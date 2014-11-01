#include "../main/precompiled.hpp"
#include "../main/singletons/epoll_daemon.hpp"
#include "../main/config_file.hpp"
#include "../main/log.hpp"
#include "../main/exception.hpp"
#include "../main/module_raii.hpp"
using namespace Poseidon;

MODULE_RAII(
	LOG_POSEIDON_INFO("Initializing HTTP server...");

	ConfigFile config("config/http_server.conf");
	return EpollDaemon::registerHttpServer(
		config.get<std::size_t>("http_server_category", 1),
		config.get<std::string>("http_server_bind", "0.0.0.0"),
		config.get<boost::uint16_t>("http_server_port", 8860),
		config.get<std::string>("http_server_certificate", "").c_str(),
		config.get<std::string>("http_server_private_key", "").c_str(),
		config.getAll<std::string>("http_server_auth_user_pass", "")
	);
)
