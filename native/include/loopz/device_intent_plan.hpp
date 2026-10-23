#pragma once
#include <loopz/device_intent_database.hpp>
#include <loopz/route_document.hpp>
#include <optional>
namespace loopz {
// Unknown never changes or queries sink ownership. Released is valid only after
// authoritative desired state, recovery receipts, sessions and default roles
// have all been examined.
enum class AppRoutingSinkDemand { Unknown, Required, Released };
struct AppRoutingSinkFacts {
    bool savedAuthoritative{}, recoveryExamined{}, desiredApplications{}, pendingReceipts{};
    bool endpointsExamined{}, endpointOccupied{}, defaultsExamined{}, defaultEndpoint{};
};
// Positive ownership evidence retains the sink even if another observation
// failed. Release requires complete desired/recovery/session/default evidence.
AppRoutingSinkDemand appRoutingSinkDemand(const AppRoutingSinkFacts&) noexcept;
// The sink shares the owner's CAS revision stream but never alters mix intents.
std::optional<DeviceIntentRequest> nextAppRoutingSinkIntent(AppRoutingSinkDemand,
                                                            const DeviceIntentSnapshot&,
                                                            std::string_view newOperationId);
// Control-thread comparison for a complete, validated, durably saved document.
// authoritative=false (missing/unreadable/unsaved document) never emits a request.
// The snapshot must belong to this authenticated user. Emits one explicit CAS
// operation; after acceptance read fresh service state before planning another.
// Re-reading accepted state makes recovery independent of a lost reply/outbox.
std::optional<DeviceIntentRequest> nextSavedDeviceIntent(const RouteDocument& document,
                                                         bool authoritative,
                                                         const DeviceIntentSnapshot& snapshot,
                                                         std::string_view newOperationId);
} // namespace loopz
