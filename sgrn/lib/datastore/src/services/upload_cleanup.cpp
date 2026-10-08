#include <drogon/orm/DbClient.h>
#include <sgrn/datastore/services/upload_cleanup.hpp>

namespace sgrn::datastore::services::upload_cleanup
{

void startUploadCleanupJob() {
    SGRN_INFO("UploadCleanup", "Starting abandoned upload session cleanup job (interval: 1 hour)");
    drogon::app().getLoop()->runInLoop([]() {
        drogon::async_run([]() -> drogon::Task<void> {
            while (true) {
                co_await drogon::sleepCoro(drogon::app().getLoop(), 3600.0); // 1 hour
                try {
                    auto db = drogon::app().getDbClient();
                    if (!db) {
                        SGRN_WARN("UploadCleanup", "No DB client available, skipping purge cycle");
                        continue;
                    }

                    // Delete expired 'active' sessions + their chunks (CASCADE)
                    auto result = co_await db->execSqlCoro("DELETE FROM storage.upload_sessions "
                                                           "WHERE status = 'active' AND expires_at < NOW()");

                    int64_t deleted = result.affectedRows();
                    if (deleted > 0) {
                        SGRN_INFO("UploadCleanup", "Purged {} abandoned upload session(s)", deleted);
                    }
                } catch (const std::exception& e) {
                    SGRN_ERROR("UploadCleanup", "Purge cycle failed: {}", e.what());
                } catch (...) {
                    SGRN_ERROR("UploadCleanup", "Purge cycle failed with unknown exception");
                }
            }
        });
    });
}

} // namespace sgrn::datastore::services::upload_cleanup