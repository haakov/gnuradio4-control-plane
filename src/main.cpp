#include "gr4cp/app/block_catalog_service.hpp"
#include "gr4cp/app/block_settings_service.hpp"
#include "gr4cp/app/scheduler_catalog_service.hpp"
#include "gr4cp/app/session_service.hpp"
#include "gr4cp/app/session_stream_service.hpp"
#include "gr4cp/api/http_server.hpp"
#include "gr4cp/catalog/gr4_block_catalog_provider.hpp"
#include "gr4cp/catalog/gr4_scheduler_catalog_provider.hpp"
#include "gr4cp/runtime/gr4_runtime_manager.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include <httplib.h>

namespace {

[[noreturn]] void report_terminate() noexcept {
    std::cerr << "gr4cp_server: fatal: std::terminate called";
    const auto exception = std::current_exception();
    if (exception) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            std::cerr << " with active exception: " << error.what();
        } catch (...) {
            std::cerr << " with an unknown active exception";
        }
    } else {
        std::cerr << " without an active exception";
    }
    std::cerr << '\n';
    std::cerr.flush();
    std::abort();
}

}  // namespace

int main() {
    std::set_terminate(report_terminate);

    try {
#if !defined(GR4CP_HAVE_GNURADIO4)
        std::cerr << "GNU Radio 4 catalog support is not built. Configure with GR4CP_ENABLE_GR4_CATALOG=ON "
                     "and a working gnuradio4 installation.\n";
        return 1;
#else
        gr4cp::storage::InMemorySessionRepository repository;
        gr4cp::runtime::Gr4RuntimeManager runtime_manager;
        gr4cp::app::SessionService session_service(repository, runtime_manager);
        gr4cp::app::SessionStreamService session_stream_service;
        gr4cp::app::BlockSettingsService block_settings_service(repository, runtime_manager);
        gr4cp::catalog::Gr4BlockCatalogProvider block_catalog_provider;
        gr4cp::app::BlockCatalogService block_catalog_service(block_catalog_provider);
        gr4cp::catalog::Gr4SchedulerCatalogProvider scheduler_catalog_provider;
        gr4cp::app::SchedulerCatalogService scheduler_catalog_service(scheduler_catalog_provider);

        try {
            (void)block_catalog_service.list();
            (void)scheduler_catalog_service.list();
            std::cout << "Using GNU Radio 4-backed block catalog\n";
            std::cout << "Using GNU Radio 4-backed scheduler catalog\n";
        } catch (const gr4cp::catalog::CatalogLoadError& error) {
            std::cerr << "GNU Radio 4 catalog initialization failed: " << error.what() << '\n';
            return 1;
        } catch (const gr4cp::catalog::SchedulerCatalogLoadError& error) {
            std::cerr << "GNU Radio 4 scheduler catalog initialization failed: " << error.what() << '\n';
            return 1;
        }

        gr4cp::api::HttpServer server(session_service,
                                      session_stream_service,
                                      block_catalog_service,
                                      scheduler_catalog_service,
                                      block_settings_service);

        const char* port_env = std::getenv("GR4CP_PORT");
        const int port = port_env != nullptr ? std::stoi(port_env) : 8080;

        std::cout << "Listening on 0.0.0.0:" << port << '\n';
        if (!server.listen("0.0.0.0", port)) {
            std::cerr << "gr4cp_server: HTTP server stopped because listen failed on 0.0.0.0:" << port << '\n';
            return 1;
        }
        return 0;
#endif
    } catch (const std::exception& error) {
        std::cerr << "gr4cp_server: fatal unhandled exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "gr4cp_server: fatal unknown exception\n";
        return 1;
    }
}
