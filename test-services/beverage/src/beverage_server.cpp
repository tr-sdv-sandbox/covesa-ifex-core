#include "beverage_server.hpp"

#include <iomanip>
#include <sstream>

#include "ifex/network.hpp"

namespace swdv {
namespace beverage_service {

using namespace std::chrono_literals;

BeverageServiceImpl::BeverageServiceImpl() {
    // Initialize service
}

void BeverageServiceImpl::Start() {
    running_ = true;
    update_thread_ = std::thread([this]() {
        while (running_) {
            UpdatePreparationProgress();
            std::this_thread::sleep_for(100ms);
        }
    });
    LOG(INFO) << "Beverage service started";
}

void BeverageServiceImpl::Stop() {
    running_ = false;
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
    LOG(INFO) << "Beverage service stopped";
}

grpc::Status BeverageServiceImpl::prepare_beverage(
    grpc::ServerContext* context,
    const prepare_beverage_request* request,
    prepare_beverage_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    // Check if already preparing
    if (current_preparation_ && 
        current_preparation_->state != preparation_state_t::READY &&
        current_preparation_->state != preparation_state_t::ERROR) {
        response->set_accepted(false);
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "Beverage preparation already in progress");
    }
    
    // Check water level
    if (water_level_percent_ < 20) {
        response->set_accepted(false);
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "Water level too low");
    }
    
    // Create new preparation
    auto prep = std::make_unique<PreparationInfo>();
    prep->preparation_id = GeneratePreparationId();
    prep->config = request->config();
    prep->state = preparation_state_t::HEATING;
    prep->start_time = std::chrono::steady_clock::now();
    prep->progress_percent = 0;
    
    uint32_t brew_time = CalculateBrewTime(prep->config);
    prep->ready_time = prep->start_time + std::chrono::seconds(brew_time);
    
    response->set_accepted(true);
    response->set_preparation_id(prep->preparation_id);
    response->set_estimated_duration_seconds(brew_time);
    
    // Store preparation
    preparations_[prep->preparation_id] = std::move(prep);
    current_preparation_ = preparations_[preparations_.rbegin()->first].get();
    
    LOG(INFO) << "Started beverage preparation " << current_preparation_->preparation_id
              << " for " << beverage_type_t_Name(request->config().type());
    
    return grpc::Status::OK;
}

grpc::Status BeverageServiceImpl::get_preparation_status(
    grpc::ServerContext* context,
    const get_preparation_status_request* request,
    get_preparation_status_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    PreparationInfo* prep = nullptr;
    
    if (request->preparation_id().empty() && current_preparation_) {
        prep = current_preparation_;
    } else if (!request->preparation_id().empty()) {
        auto it = preparations_.find(request->preparation_id());
        if (it != preparations_.end()) {
            prep = it->second.get();
        }
    }
    
    if (!prep) {
        auto* status = response->mutable_status();
        status->set_state(preparation_state_t::IDLE);
        status->set_progress_percent(0);
        status->set_remaining_seconds(0);
        return grpc::Status::OK;
    }
    
    auto* status = response->mutable_status();
    status->set_state(prep->state);
    status->set_progress_percent(prep->progress_percent);
    
    if (prep->state == preparation_state_t::READY || 
        prep->state == preparation_state_t::ERROR) {
        status->set_remaining_seconds(0);
    } else {
        auto now = std::chrono::steady_clock::now();
        auto remaining = prep->ready_time - now;
        status->set_remaining_seconds(
            std::chrono::duration_cast<std::chrono::seconds>(remaining).count());
        status->set_ready_at_timestamp(
            std::chrono::duration_cast<std::chrono::seconds>(
                prep->ready_time.time_since_epoch()).count());
    }
    
    return grpc::Status::OK;
}

grpc::Status BeverageServiceImpl::cancel_preparation(
    grpc::ServerContext* context,
    const cancel_preparation_request* request,
    cancel_preparation_response* response) {
    
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    auto it = preparations_.find(request->preparation_id());
    if (it == preparations_.end()) {
        response->set_success(false);
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Preparation not found");
    }
    
    if (it->second->state == preparation_state_t::READY ||
        it->second->state == preparation_state_t::ERROR) {
        response->set_success(false);
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "Cannot cancel completed preparation");
    }
    
    it->second->state = preparation_state_t::ERROR;
    response->set_success(true);
    
    LOG(INFO) << "Cancelled beverage preparation " << request->preparation_id();
    
    return grpc::Status::OK;
}

grpc::Status BeverageServiceImpl::get_capabilities(
    grpc::ServerContext* context,
    const get_capabilities_request* request,
    get_capabilities_response* response) {
    
    // Add all available beverage types
    response->add_available_types(beverage_type_t::COFFEE);
    response->add_available_types(beverage_type_t::ESPRESSO);
    response->add_available_types(beverage_type_t::CAPPUCCINO);
    response->add_available_types(beverage_type_t::TEA);
    response->add_available_types(beverage_type_t::HOT_WATER);
    
    response->set_water_level_percent(water_level_percent_.load());
    response->set_requires_maintenance(requires_maintenance_.load());
    
    return grpc::Status::OK;
}

uint32_t BeverageServiceImpl::CalculateBrewTime(const beverage_config_t& config) const {
    // Base times by beverage type (seconds)
    uint32_t base_time = 120; // 2 minutes default
    
    switch (config.type()) {
        case beverage_type_t::ESPRESSO:
            base_time = 60;  // 1 minute
            break;
        case beverage_type_t::COFFEE:
            base_time = 180; // 3 minutes
            break;
        case beverage_type_t::CAPPUCCINO:
            base_time = 150; // 2.5 minutes
            break;
        case beverage_type_t::TEA:
            base_time = 90;  // 1.5 minutes
            break;
        case beverage_type_t::HOT_WATER:
            base_time = 45;  // 45 seconds
            break;
    }
    
    // Adjust for size
    if (config.size_ml() > 350) {
        base_time += 30; // Extra time for large sizes
    }
    
    // Add heating time (simulate cold start)
    base_time += 30; // 30 seconds to heat up
    
    return base_time;
}

std::string BeverageServiceImpl::GeneratePreparationId() {
    uint32_t id = preparation_counter_.fetch_add(1);
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << "prep_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S_")
       << std::setfill('0') << std::setw(4) << id;
    
    return ss.str();
}

void BeverageServiceImpl::UpdatePreparationProgress() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    if (!current_preparation_ || 
        current_preparation_->state == preparation_state_t::READY ||
        current_preparation_->state == preparation_state_t::ERROR) {
        return;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - current_preparation_->start_time;
    auto total_duration = current_preparation_->ready_time - current_preparation_->start_time;
    
    // Calculate progress
    double progress = static_cast<double>(elapsed.count()) / total_duration.count();
    current_preparation_->progress_percent = 
        static_cast<uint32_t>(std::min(progress * 100.0, 99.0));
    
    // Update state based on progress
    if (progress < 0.3) {
        current_preparation_->state = preparation_state_t::HEATING;
    } else if (progress < 0.9) {
        current_preparation_->state = preparation_state_t::BREWING;
    } else if (now >= current_preparation_->ready_time) {
        current_preparation_->state = preparation_state_t::READY;
        current_preparation_->progress_percent = 100;
        
        // Simulate water usage
        water_level_percent_ -= 10;
        
        LOG(INFO) << "Beverage preparation " << current_preparation_->preparation_id 
                  << " completed";
    }
}

bool BeverageServiceImpl::RegisterWithDiscovery(const std::string& discovery_endpoint, 
                                               int port, 
                                               const std::string& ifex_schema) {
    try {
        LOG(INFO) << "Registering Beverage Service with discovery on port " << port;
        
        // Get primary IP address instead of using localhost
        std::string primary_ip = ifex::network::get_primary_ip_address();
        if (primary_ip.empty()) {
            LOG(WARNING) << "Could not determine primary IP address, falling back to localhost";
            primary_ip = "localhost";
        }
        
        // Create channel to discovery service
        auto channel = grpc::CreateChannel(discovery_endpoint, grpc::InsecureChannelCredentials());
        auto stub = swdv::service_discovery::register_service_service::NewStub(channel);
        
        // Create service info
        swdv::service_discovery::register_service_request request;
        auto* service_info = request.mutable_service_info();
        
        service_info->set_name("beverage_service");
        service_info->set_version("1.0.0");
        service_info->set_description("In-vehicle beverage preparation system");
        service_info->set_status(swdv::service_discovery::service_status_t::AVAILABLE);
        service_info->set_ifex_schema(ifex_schema);
        
        // Set endpoint with actual IP address
        auto* endpoint = service_info->mutable_endpoint();
        endpoint->set_address(primary_ip + ":" + std::to_string(port));
        endpoint->set_transport(swdv::service_discovery::transport_type_t::GRPC);
        
        LOG(INFO) << "Registering with endpoint: " << endpoint->address();
        
        // Make the call
        swdv::service_discovery::register_service_response response;
        grpc::ClientContext context;
        
        auto status = stub->register_service(&context, request, &response);
        
        if (status.ok() && !response.registration_id().empty()) {
            registration_id_ = response.registration_id();
            LOG(INFO) << "Successfully registered with service discovery, ID: " << registration_id_;
            return true;
        } else {
            LOG(ERROR) << "Failed to register with service discovery: " << status.error_message();
            return false;
        }
        
    } catch (const std::exception& e) {
        LOG(ERROR) << "Registration failed: " << e.what();
        return false;
    }
}

} // namespace beverage_service
} // namespace swdv