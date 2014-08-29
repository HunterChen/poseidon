#include "../precompiled.hpp"
#include "http_session.hpp"
#include "http_status.hpp"
#include "log.hpp"
#include "singletons/job_dispatcher.hpp"
#include "singletons/http_servlet_manager.hpp"
#include "stream_buffer.hpp"
#include "utilities.hpp"
#include "exception.hpp"
using namespace Poseidon;

namespace {

const unsigned long long MAX_REQUEST_LENGTH = 0x4000;	// 头部加正文总长度。

void respond(const boost::shared_ptr<HttpSession> &session, HttpStatus status,
	OptionalMap *headers = NULL, std::string *contents = NULL)
{
	LOG_DEBUG("Sending HTTP response: status = ", (unsigned)status);

	const AUTO(desc, getHttpStatusCodeDesc(status));
	const AUTO(codeStatus, boost::lexical_cast<std::string>((unsigned)status) + ' ' + desc.descShort);

	OptionalMap emptyHeaders;
	if(!headers){
		headers = &emptyHeaders;
	}

	std::string emptyContents;
	if(!contents){
		contents = &emptyContents;
	}

	if(contents->empty() && ((unsigned)status / 100 != 2)){
		*contents = "<html><head><title>";
		*contents += codeStatus;
		*contents += "</title></head><body><h1>";
		*contents += codeStatus;
		*contents += "</h1><hr /><p>";
		*contents += desc.descLong;
		*contents += "</p></body></html>";

		headers->set("Content-Type", "text/html");
	} else {
		AUTO_REF(contentType, headers->create("Content-Type"));
		if(contentType.empty()){
			contentType.assign("text/plain; charset=utf-8");
		}
	}
	headers->set("Content-Length", boost::lexical_cast<std::string>(contents->size()));

	StreamBuffer buffer;
	buffer.put("HTTP/1.1 ", 9);
	buffer.put(codeStatus.data(), codeStatus.size());
	buffer.put("\r\n", 2);
	for(AUTO(it, headers->begin()); it != headers->end(); ++it){
		if(!it->second.empty()){
			buffer.put(it->first.get(), std::strlen(it->first.get()));
			buffer.put(": ", 2);
			buffer.put(it->second.data(), it->second.size());
			buffer.put("\r\n", 2);
		}
	}
	buffer.put("\r\n", 2);
	buffer.put(contents->data(), contents->size());

	session->sendWithMove(buffer);
}

class HttpErrorJob : public JobBase {
private:
	const boost::weak_ptr<HttpSession> m_session;
	const HttpStatus m_status;

public:
	HttpErrorJob(boost::weak_ptr<HttpSession> session, HttpStatus status)
		: m_session(session), m_status(status)
	{
	}

protected:
	void perform() const {
		const AUTO(session, m_session.lock());
		if(!session){
			LOG_WARNING("The specified HTTP session has expired.");
			return;
		}
		respond(session, m_status);
	}
};

void kill(const boost::shared_ptr<HttpSession> &session, HttpStatus status){
	boost::make_shared<HttpErrorJob>(session, status)->pend();
	session->shutdownRead();
}

HttpVerb translateVerb(const std::string &verb){
	switch(verb.size()){
	case 3:
		if(verb == "GET"){
			return HTTP_GET;
		} else if(verb == "PUT"){
			return HTTP_PUT;
		}
		break;

	case 4:
		if(verb == "POST"){
			return HTTP_POST;
		} else if(verb == "HEAD"){
			return HTTP_HEAD;
		}
		break;

	case 5:
		if(verb == "TRACE"){
			return HTTP_TRACE;
		}
		break;

	case 6:
		if(verb == "DELETE"){
			return HTTP_DELETE;
		}
		break;

	case 7:
		if(verb == "CONNECT"){
			return HTTP_CONNECT;
		} else if(verb == "OPTIONS"){
			return HTTP_OPTIONS;
		}
		break;
	}
	return HTTP_INVALID_VERB;
}

int getHexLiteral(char ch){
	if(('0' <= ch) && (ch <= '9')){
		return ch - '0';
	} else if(('A' <= ch) && (ch <= 'F')){
		return ch - 'A' + 0x0A;
	} else if(('a' <= ch) && (ch <= 'f')){
		return ch - 'a' + 0x0A;
	}
	return -1;
}

std::string urlDecode(const std::string &source){
	std::string ret;
	const std::size_t size = source.size();
	ret.reserve(size);
	std::size_t i = 0;
	while(i < size){
		const char ch = source[i];
		++i;
		if(ch == '+'){
			ret.push_back(' ');
			continue;
		}
		if((ch != '%') || ((i + 1) >= size)){
			ret.push_back(ch);
			continue;
		}
		const int high = getHexLiteral(source[i]);
		const int low = getHexLiteral(source[i + 1]);
		if((high == -1) || (low == -1)){
			ret.push_back(ch);
			continue;
		}
		i += 2;
		ret.push_back((high << 4) | low);
	}
	return ret;
}

OptionalMap optionalMapFromUrlEncoded(const std::string &encoded){
	OptionalMap ret;
	const AUTO(parts, split<std::string>(encoded, '&'));
	for(AUTO(it, parts.begin()); it != parts.end(); ++it){
		const std::size_t pos = it->find('=');
		if(pos == std::string::npos){
			ret.set(urlDecode(*it), std::string());
		} else {
			ret.set(urlDecode(it->substr(0, pos)), urlDecode(it->substr(pos + 1)));
		}
	}
	return ret;
}

class HttpRequestJob : public JobBase {
private:
	const boost::weak_ptr<HttpSession> m_session;

	const HttpVerb m_verb;
	const std::string m_uri;
	const OptionalMap m_getParams;
	const OptionalMap m_postParams;

public:
	HttpRequestJob(boost::weak_ptr<HttpSession> session, HttpVerb verb,
		std::string uri, OptionalMap getParams, OptionalMap postParams)
		: m_session(session), m_verb(verb)
		, m_uri(uri), m_getParams(getParams), m_postParams(postParams)
	{
	}

protected:
	void perform() const {
		const AUTO(session, m_session.lock());
		if(!session){
			LOG_WARNING("The specified HTTP session has expired.");
			return;
		}

		const AUTO(servlet, HttpServletManager::getServlet(m_uri));
		if(!servlet){
			LOG_WARNING("No servlet for URI ", m_uri);
			respond(session, HTTP_NOT_FOUND);
			return;
		}
		OptionalMap headers;
		std::string contents;
		try {
			const HttpStatus status = (*servlet)(headers, contents,
				m_verb, m_getParams, m_postParams);
			respond(session, status, &headers, &contents);
		} catch(ProtocolException &e){
			LOG_ERROR("ProtocolException thrown in HTTP servlet, code = ", e.code(),
				", file = ", e.file(), ", line = ", e.line(), ", what = ", e.what());
			if(e.code() > 0){
				respond(session, (HttpStatus)e.code());
			}
		}
	}
};

}

HttpSession::HttpSession(ScopedFile &socket)
	: TcpSessionBase(socket)
	, m_headerIndex(0), m_totalLength(0), m_contentLength(0), m_line()
{
}

void HttpSession::onReadAvail(const void *data, std::size_t size){
	if(m_totalLength + size >= MAX_REQUEST_LENGTH){
		kill(virtualSharedFromThis<HttpSession>(), HTTP_REQUEST_TOO_LARGE);
		return;
	}
	m_totalLength += size;

	AUTO(read, (const char *)data);
	const AUTO(end, read + size);
	while(read != end){
		if(m_headerIndex != -1){	// -1 表示正文。
			const char ch = *read;
			++read;
			if(ch != '\n'){
				m_line.push_back(ch);
				continue;
			}
			const std::size_t lineLen = m_line.size();
			if((lineLen > 0) && (m_line[lineLen - 1] == '\r')){
				m_line.resize(lineLen - 1);
			}
			if(m_headerIndex == 0){
				if(m_line.empty()){
					continue;
				}
				std::string verb;
				std::string uri;
				std::string version;
				std::istringstream ss(m_line);
				if(!(ss >>verb >>uri >>version)){
					LOG_WARNING("Bad HTTP header: ", m_line);
					kill(virtualSharedFromThis<HttpSession>(), HTTP_BAD_REQUEST);
					return;
				}
				m_verb = translateVerb(verb);
				if(m_verb == HTTP_INVALID_VERB){
					LOG_WARNING("Bad HTTP verb: ", verb);
					kill(virtualSharedFromThis<HttpSession>(), HTTP_BAD_METHOD);
					return;
				}
				if((version != "HTTP/1.0") && (version != "HTTP/1.1")){
					LOG_WARNING("Bad HTTP version: ", version);
					kill(virtualSharedFromThis<HttpSession>(), HTTP_VERSION_NOT_SUP);
					return;
				}
				const std::size_t questionPos = uri.find('?');
				if(questionPos == std::string::npos){
					m_uri = uri;
					m_getParams.clear();
				} else {
					m_uri = uri.substr(0, questionPos);
					m_getParams = optionalMapFromUrlEncoded(uri.substr(questionPos + 1));
				}
				++m_headerIndex;
			} else if(!m_line.empty()){
				const std::size_t delimPos = m_line.find(": ");
				if(delimPos == std::string::npos){
					LOG_WARNING("Bad HTTP header: ", m_line);
					kill(virtualSharedFromThis<HttpSession>(), HTTP_BAD_REQUEST);
					return;
				}
				if(m_line.compare(0, delimPos, "Content-Length") == 0){
					m_contentLength = boost::lexical_cast<std::size_t>(m_line.substr(delimPos + 2));
				} else if(m_line.compare(0, delimPos, "Content-Type") == 0){
					if(m_line.compare(delimPos + 2, std::string::npos,
						"application/x-www-form-urlencoded", 33) != 0)
					{
						LOG_WARNING("Unexpected HTTP Content-Type: ", m_line.substr(delimPos + 2));
						kill(virtualSharedFromThis<HttpSession>(), HTTP_UNSUPPORTED_MEDIA);
						return;
					}
				}
				++m_headerIndex;
			} else if(m_contentLength == 0){
				// 没有正文。
				boost::make_shared<HttpRequestJob>(
					virtualWeakFromThis<HttpSession>(),
					m_verb, m_uri, m_getParams, m_postParams
					)->pend();
				m_headerIndex = 0;
				m_totalLength = 0;
			} else {
				m_headerIndex = -1;
			}
			m_line.clear();
		} else {
			const std::size_t bytesAvail = (std::size_t)(end - read);
			const std::size_t bytesRemaining = m_contentLength - m_line.size();
			if(bytesAvail < bytesRemaining){
				m_line.append(read, bytesAvail);
				read += bytesAvail;
				continue;
			}
			m_line.append(read, bytesRemaining);
			read += bytesRemaining;

			m_postParams = optionalMapFromUrlEncoded(m_line);

			boost::make_shared<HttpRequestJob>(
				virtualWeakFromThis<HttpSession>(),
				m_verb, m_uri, m_getParams, m_postParams
				)->pend();
			m_headerIndex = 0;
			m_totalLength = 0;
			m_contentLength = 0;
			m_line.clear();
		}
	}
}
void HttpSession::onReadComplete(){
	if(m_headerIndex != 0){
		LOG_WARNING("Now that this connection has been shutdown, "
			"an incomplete request has been discarded.");
	}
}