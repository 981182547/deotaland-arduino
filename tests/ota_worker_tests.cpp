// Compile the real implementation through deterministic platform boundaries.
#include "../src/ota_service.cpp"
#include <cstdio>
namespace {
unsigned checks=0;
#define CHECK(v) do { ++checks; if (!(v)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#v); std::exit(1); } } while (0)
std::vector<agent_ota_status_t> callbacks;
std::vector<uint8_t> phases;
unsigned event_calls=0;
bool reject_events=false, disconnect_on_event=false;
uint8_t expected_sha[32]={};
void Callback(const agent_ota_status_t* status,void* context) {
    CHECK(context==&callbacks); callbacks.push_back(*status);
}
esp_err_t Event(uint8_t id,const uint8_t* data,size_t bytes) {
    CHECK(id==0x38 && bytes==11); ++event_calls;
    if (disconnect_on_event) { disconnect_on_event=false; agentlink::ota::OnLinkState(false); }
    if (reject_events) return ESP_ERR_NO_MEM;
    phases.push_back(data[0]); return ESP_OK;
}
void Poll() { agentlink::ota::Poll(); }
void RunWorker() {
    try { fake::task->function(fake::task->context); CHECK(false); }
    catch (const fake::IdleWait&) {}
}
agent_ota_status_t Status() { agent_ota_status_t status={}; agent_link_ota_get_status(&status); return status; }
void NewConnection() {
    agentlink::ota::OnLinkState(false);
    agentlink::ota::OnLinkState(true);
    agentlink::ota::Configure(true,true);
    fake::ticks=0; fake::receive_limit=SIZE_MAX; fake::begin_calls=0;
    fake::end_calls=0; fake::abort_calls=0; fake::restarts=0; fake::receive_calls=0;
    fake::begin_failure=false; fake::end_failure=false; fake::bad_hash=false;
    fake::write_failure=0; fake::writes.clear();
    fake::next_partition=&fake::target; fake::boot_partition=&fake::previous;
    fake::delay_hook=nullptr; fake::receive_hook=nullptr; fake::write_hook=nullptr;
    callbacks.clear(); phases.clear(); event_calls=0; reject_events=false; disconnect_on_event=false;
    agent_link_ota_set_callback(Callback,&callbacks);
    agentlink::ota::SetReliableEventSink(Event);
}
std::vector<uint8_t> Image(size_t size) {
    std::vector<uint8_t> image(size);
    for (size_t i=0;i<size;++i) image[i]=static_cast<uint8_t>((i*59u+i/251u)&255u);
    image[0]=0xe9; return image;
}
bool Start(size_t size,uint16_t* error=nullptr) {
    return agentlink::ota::Start(static_cast<uint32_t>(size),expected_sha,"2.0.0",nullptr,error);
}
void TestPreparation() {
    using namespace agentlink::ota;
    Configure(false,true); CHECK(agent_link_ota_prepare()==ESP_ERR_NOT_SUPPORTED);
    Configure(true,false); CHECK(agent_link_ota_prepare()==ESP_ERR_NOT_SUPPORTED);
    Configure(true,true); fake::next_partition=nullptr;
    CHECK(agent_link_ota_prepare()==ESP_ERR_NOT_FOUND && fake::stream_creates==0);
    fake::next_partition=&fake::target;
    for (unsigned failure=1;failure<=4;++failure) {
        fake::allocation_failure=failure;
        CHECK(agent_link_ota_prepare()==ESP_ERR_NO_MEM);
    }
    fake::allocation_failure=0; CHECK(agent_link_ota_prepare()==ESP_OK);
    CHECK(fake::stream_creates==2 && fake::queue_creates==2 && fake::chunk_creates==2 && fake::task_creates==2);
    for (unsigned i=0;i<10;++i) CHECK(agent_link_ota_prepare()==ESP_OK);
    CHECK(fake::stream_creates==2 && fake::queue_creates==2 && fake::chunk_creates==2 && fake::task_creates==2);
    CHECK(!agent_link_ota_is_busy() && !OwnsDataChannel() && fake::task->notifications==0);
    CHECK(!WorkerActive()); // An idle prewarmed task must not lock out begin().
}
void TestBoundedTerminalProgress() {
    using namespace agentlink::ota;
    NewConnection(); const auto image=Image(10000); fake::receive_limit=73; fake::bad_hash=true;
    CHECK(Start(image.size()) && fake::task->notifications==1);
    CHECK(WorkerActive()); // Protects the window before the worker is scheduled.
    CHECK(FeedData(image.data(),image.size())==image.size()); RunWorker();
    CHECK(!IsBusy() && Status().state==AGENT_OTA_FAILED && Status().error_code==kErrSha256Mismatch);
    CHECK(!WorkerActive());
    CHECK(fake::begin_size==OTA_WITH_SEQUENTIAL_WRITES && fake::abort_calls==1 && fake::end_calls==0);
    CHECK(fake::writes.size()==3 && fake::writes[0].size()==4096 && fake::writes[1].size()==4096 && fake::writes[2].size()==1808);
    std::vector<uint8_t> written;
    for (const auto& chunk:fake::writes) written.insert(written.end(),chunk.begin(),chunk.end());
    CHECK(written==image);
    Poll(); CHECK(event_calls==1 && callbacks.size()==1 && phases.back()==0);
    Poll(); CHECK(event_calls==2 && callbacks.size()==2 && phases.back()==1);
    reject_events=true; Poll(); CHECK(event_calls==3 && callbacks.size()==3);
    CHECK(callbacks.back().state==AGENT_OTA_FAILED);
    fake::ticks=199; Poll(); CHECK(event_calls==3 && callbacks.size()==3);
    fake::ticks=200; Poll(); CHECK(event_calls==4 && callbacks.size()==3);
    reject_events=false; fake::ticks=400; Poll(); CHECK(event_calls==5 && callbacks.size()==3 && phases.back()==3);
    Poll(); CHECK(event_calls==5 && callbacks.size()==3);
    CHECK(OwnsDataChannel() && FeedData(image.data(),200)==200);
    CHECK(!Start(image.size())); // Reconnect is still mandatory after a terminal session.
    CHECK(fake::task_creates==2 && fake::chunk_creates==2);
}
void TestSuccessfulReuse() {
    using namespace agentlink::ota;
    for (unsigned session=0;session<2;++session) {
        NewConnection(); const auto image=Image(4097); fake::receive_limit=23;
        CHECK(Start(image.size())); CHECK(FeedData(image.data(),image.size())==image.size());
        fake::delay_hook=Poll; RunWorker(); fake::delay_hook=nullptr;
        CHECK(Status().state==AGENT_OTA_SUCCESS && fake::restarts==1 && fake::ticks>=2000);
        CHECK(fake::end_calls==1 && fake::abort_calls==0 && fake::boot_partition==&fake::target);
        CHECK(callbacks.size()==3 && phases==std::vector<uint8_t>({0,1,2}));
        CHECK(fake::writes.size()==2 && fake::writes.back().size()==1);
        CHECK(fake::task_creates==2 && fake::chunk_creates==2 && fake::task->notifications==0);
    }
}
void TestErrorsAndCancellation() {
    using namespace agentlink::ota;
    NewConnection(); uint16_t error=0;
    CHECK(!Start(0,&error) && error==kErrInvalidPayload);
    CHECK(!Start(fake::target.size+1,&error) && error==kErrFirmwareTooLarge);
    fake::boot_partition=nullptr; CHECK(!Start(10,&error) && error==kErrPartitionFail);
    fake::boot_partition=&fake::previous;
    CHECK(Start(512)); const auto image=Image(513);
    CHECK(FeedData(image.data(),513)==513); RunWorker();
    CHECK(Status().state==AGENT_OTA_FAILED && Status().error_code==kErrInvalidPayload && fake::writes.empty());
    NewConnection(); CHECK(Start(8192));
    const auto large=Image(8192); CHECK(FeedData(large.data(),large.size())==large.size());
    fake::write_failure=1; RunWorker();
    CHECK(Status().error_code==kErrWriteFail && fake::abort_calls==1 && fake::writes.size()==1);
    NewConnection(); CHECK(Start(8192)); CHECK(FeedData(large.data(),large.size())==large.size());
    agent_link_ota_abort(); RunWorker(); CHECK(Status().error_code==kErrAborted && fake::writes.empty());
    NewConnection(); CHECK(Start(8192)); CHECK(FeedData(large.data(),large.size())==large.size());
    OnLinkState(false);
    CHECK(!IsBusy() && WorkerActive());
    CHECK(!Start(8192));
    RunWorker(); CHECK(Status().state==AGENT_OTA_IDLE && fake::writes.empty());
    CHECK(!WorkerActive());
    Poll(); CHECK(callbacks.empty() && phases.empty());
}
void TestQueuePressureAndCallbackGeneration() {
    using namespace agentlink::ota;
    NewConnection(); const auto large=Image(40000); CHECK(Start(large.size()));
    CHECK(FeedData(large.data(),32000)==32000);
    CHECK(!WantsMore() && FeedData(large.data()+32000,8000)==0);
    CHECK(Status().received_bytes==32000);
    OnLinkState(false); RunWorker();
    NewConnection(); const auto image=Image(100); fake::bad_hash=true;
    CHECK(Start(image.size())); CHECK(FeedData(image.data(),image.size())==image.size()); RunWorker();
    disconnect_on_event=true; Poll(); CHECK(event_calls==1 && callbacks.empty());
    Poll(); CHECK(event_calls==1 && callbacks.empty() && Status().state==AGENT_OTA_IDLE);
}
void CancelSuccess() {
    agentlink::ota::Poll();
    if (Status().state==AGENT_OTA_SUCCESS) agentlink::ota::OnLinkState(false);
}
void TestSuccessCancellation() {
    NewConnection(); const auto image=Image(100);
    CHECK(Start(image.size())); CHECK(agentlink::ota::FeedData(image.data(),image.size())==image.size());
    fake::delay_hook=CancelSuccess; RunWorker(); fake::delay_hook=nullptr;
    CHECK(fake::restarts==0 && fake::boot_partition==&fake::previous);
    CHECK(Status().state==AGENT_OTA_IDLE && fake::end_calls==1);
}
void TestLazyPreparation() {
    NewConnection(); const auto image=Image(200); fake::bad_hash=true;
    CHECK(Start(image.size()));
    CHECK(fake::stream_creates==1 && fake::queue_creates==1 && fake::chunk_creates==1 && fake::task_creates==1);
    CHECK(agentlink::ota::FeedData(image.data(),image.size())==image.size()); RunWorker();
    CHECK(Status().error_code==agentlink::ota::kErrSha256Mismatch);
    CHECK(agent_link_ota_prepare()==ESP_OK && fake::task_creates==1);
}
void CancelDuringFlash() {
    using namespace agentlink::ota;
    CHECK(WorkerActive());
    OnLinkState(false);
    CHECK(!IsBusy() && WorkerActive());
    CHECK(!Start(8192));
}
void TestWorkerLifetimeDuringFlash() {
    NewConnection(); const auto image=Image(8192);
    CHECK(Start(image.size())); CHECK(agentlink::ota::FeedData(image.data(),image.size())==image.size());
    fake::write_hook=CancelDuringFlash; RunWorker(); fake::write_hook=nullptr;
    CHECK(!agentlink::ota::WorkerActive() && fake::abort_calls==1);
    CHECK(Status().state==AGENT_OTA_IDLE && fake::boot_partition==&fake::previous);
    // The same prewarmed task and resources are usable once cleanup completes.
    CHECK(Start(100)); agentlink::ota::Abort("test finished"); RunWorker();
    CHECK(!agentlink::ota::WorkerActive() && fake::task_creates==2);
}
}
extern "C" uint32_t agent_link_now_ms(void) { return fake::ticks; }
int main(int argc,char**) {
    if (argc>1) { TestLazyPreparation(); std::printf("PASS %u OTA lazy-start checks\n",checks); return 0; }
    TestPreparation(); TestBoundedTerminalProgress(); TestSuccessfulReuse();
    TestErrorsAndCancellation(); TestQueuePressureAndCallbackGeneration(); TestSuccessCancellation();
    TestWorkerLifetimeDuringFlash();
    std::printf("PASS %u OTA worker checks\n",checks);
}
