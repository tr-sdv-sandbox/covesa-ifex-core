#pragma once

#include <grpcpp/grpcpp.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/empty.pb.h>
#include <nlohmann/json.hpp>
#include <glog/logging.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "departure-orchestration-service.grpc.pb.h"
#include "service-discovery-service.grpc.pb.h"
#include "ifex-dispatcher-service.grpc.pb.h"
#include <ifex/parser.hpp>
#include <yaml-cpp/yaml.h>

namespace swdv {
namespace departure_orchestration_service {

struct ScheduleEntry {
    std::string schedule_id;
    departure_schedule_t schedule;
    schedule_status_t status;
    int64_t preparation_start_time;
    std::string current_step;
    uint8_t completion_percent;
    std::vector<preparation_step_t> timeline;
    std::thread preparation_thread;
};

class DepartureOrchestrationServiceImpl final 
    : public schedule_departure_service::Service,
      public get_schedule_status_service::Service,
      public cancel_schedule_service::Service,
      public modify_schedule_service::Service,
      public get_active_schedules_service::Service {
public:
    DepartureOrchestrationServiceImpl();
    ~DepartureOrchestrationServiceImpl();

    // gRPC service methods
    grpc::Status schedule_departure(
        grpc::ServerContext* context,
        const schedule_departure_request* request,
        schedule_departure_response* response) override;

    grpc::Status get_schedule_status(
        grpc::ServerContext* context,
        const get_schedule_status_request* request,
        get_schedule_status_response* response) override;

    grpc::Status cancel_schedule(
        grpc::ServerContext* context,
        const cancel_schedule_request* request,
        cancel_schedule_response* response) override;

    grpc::Status modify_schedule(
        grpc::ServerContext* context,
        const modify_schedule_request* request,
        modify_schedule_response* response) override;

    grpc::Status get_active_schedules(
        grpc::ServerContext* context,
        const get_active_schedules_request* request,
        get_active_schedules_response* response) override;

    // Service lifecycle
    void Start();
    void Stop();

    // Service discovery
    bool RegisterWithDiscovery(const std::string& discovery_endpoint, 
                             int port, 
                             const std::string& service_name,
                             const std::string& ifex_schema);
    
    // Connect to dispatcher
    void SetDispatcherEndpoint(const std::string& dispatcher_endpoint);
    
    // Schema discovery and aggregation
    void DiscoverDependentSchemas();
    std::string BuildCompositeSchema();

private:
    // Schedule management
    std::vector<preparation_step_t> CreatePreparationTimeline(
        const departure_schedule_t& schedule);
    
    void ExecuteSchedule(const std::string& schedule_id);
    void ExecutePreparationStep(const std::string& schedule_id, 
                               const preparation_step_t& step);
    
    std::string GenerateScheduleId();
    int64_t GetCurrentTimestamp();
    
    // Service coordination through dispatcher
    bool CallServiceMethod(const std::string& service_name,
                          const std::string& namespace_name,
                          const std::string& method_name,
                          const nlohmann::json& params);
    
    bool StartClimateComfort(float temperature, bool seat_heating, bool steering_wheel_heating);
    bool StartDefrost();
    bool PrepareBeverage(const beverage_request_t& request);
    bool PreconditionBattery();
    bool CalculateRoute(const std::string& destination);
    
    // State management
    std::mutex schedules_mutex_;
    std::unordered_map<std::string, std::unique_ptr<ScheduleEntry>> schedules_;
    std::atomic<uint32_t> schedule_counter_{0};
    std::atomic<bool> running_{false};
    
    // Service discovery
    std::shared_ptr<swdv::service_discovery::service_info_t> service_info_;
    std::unique_ptr<swdv::service_discovery::heartbeat_service::Stub> heartbeat_stub_;
    std::thread heartbeat_thread_;
    std::string service_id_;
    
    // Dispatcher endpoint
    std::string dispatcher_endpoint_;
    
    // Schema aggregation
    struct DependentService {
        std::string name;
        std::string ifex_schema;
        std::unique_ptr<ifex::Parser> parser;
    };
    std::map<std::string, DependentService> dependent_services_;
    std::string composite_schema_;
    std::mutex schema_mutex_;
};

} // namespace departure_orchestration_service
} // namespace swdv