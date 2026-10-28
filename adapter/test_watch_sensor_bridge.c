/* Validate the actual ARM adapter's sample conversion and batching. */
#define sendto capture_sendto
#define main adapter_main
#include "watch_device_bridge.c"
#undef main
#undef sendto
#include <assert.h>
#undef assert
#define assert(test) do { if (!(test)) { fprintf(stderr,"CHECK_FAILED line=%d\n",__LINE__); return 1; } } while (0)
static unsigned sent;
static struct pbl_adapter_sensor captured;
// The latest message that is not a sensor batch: CAPS, STATE, FAULT, GESTURE.
static uint8_t other[256];
static size_t other_size;
static unsigned other_sent;
static unsigned stops, destroys, creates, starts, batch_ms, callback_ms;
static int start_result;
static unsigned power_failure;
static int fake_wake(bool lock) {
  if (lock) abort();
  return power_failure == 1 ? -EIO : 0;
}
static int fake_display_lock(int type, int timeout) {
  if (type != 1 || timeout) abort();
  return power_failure == 2 ? -EIO : 0;
}
static int fake_display_unlock(int type) {
  if (type != 1) abort();
  return power_failure == 3 ? -EIO : 0;
}
static int fake_display_state(int state) {
  if (state != 2) abort();
  return power_failure == 4 ? -EIO : 0;
}
static int fake_get(const char *uri, void **sensor) {
  if (!strstr(uri, "SENSOR_ACCELEROMETER")) abort();
  *sensor = (void *)3;
  return 0;
}
static int fake_create(void *sensor, void **listener) {
  if (sensor != (void *)3) abort();
  *listener = (void *)4;
  creates++;
  return 0;
}
static int fake_cb(void *listener, unsigned interval,
                   void (*callback)(void *, void *, void *), void *context) {
  if (listener != (void *)4 || callback != health_event || context) abort();
  callback_ms = interval;
  return 0;
}
static int fake_events_cb(void *listener,
                          void (*callback)(void *, struct sensor_event *, int, void *),
                          void *context) {
  if (listener != (void *)4 || callback != health_events || context) abort();
  // Registering a bulk callback resets this vendor API's interval to its default.
  callback_ms = 100;
  return 0;
}
static int fake_interval(void *listener, unsigned interval) {
  if (listener != (void *)4 || callback_ms != 100) abort();
  callback_ms = interval;
  return 0;
}
static int fake_option(void *listener, int option) {
  if (listener != (void *)4 || option != 1) abort();
  return 0;
}
static int fake_batch(void *listener, unsigned latency) {
  if (listener != (void *)4) abort();
  batch_ms = latency;
  return 0;
}
static int fake_start(void *listener) {
  if (listener != (void *)4) abort();
  starts++;
  return start_result;
}
static int fake_stop(void *listener) { (void)listener; stops++; return 0; }
static float fake_total = 1000;
static int fake_read_result;
static int fake_read(void *listener, struct sensor_event *event) {
  if (listener != (void *)4) abort();
  *event = (struct sensor_event){.accuracy = 2, .count = 8, .values = {fake_total}};
  return fake_read_result;
}
static int fake_destroy(void *listener) { (void)listener; destroys++; return 0; }
ssize_t capture_sendto(int fd, const void *buffer, size_t length, int flags,
                      const struct sockaddr *address, socklen_t size) {
  (void)fd; (void)flags; (void)address; (void)size;
  if (length != sizeof(captured)) {
    if (length > sizeof(other)) return -1;
    memcpy(other, buffer, length); other_size = length; other_sent++;
    return length;
  }
  memcpy(&captured,buffer,length); sent++; return length;
}
int main(void) {
  s_health.fd = 3;  // any descriptor: sendto is captured
  s_health.running[0]=true; s_health.running[1]=true;
  s_health.samples=125; s_health.interval_ms=40;
  struct sensor_event e={.accuracy=2,.timestamp=1000000,.count=3,.values={9.80665f,-9.80665f,9.80665f}};
  for (unsigned i=0;i<124;i++) { health_event(NULL,&e,NULL); e.timestamp+=40000; }
  assert(sent==0);
  health_event(NULL,&e,NULL);
  assert(sent==1 && captured.hdr.major==PBL_ADAPTER_MAJOR && captured.hdr.magic==PBL_ADAPTER_MAGIC &&
         captured.hdr.type==PBL_ADAPTER_SENSOR && captured.hdr.length==sizeof(captured) &&
         captured.type==PBL_ADAPTER_SENSOR_ACCEL && captured.count==125 &&
         captured.timestamp_us==1000000);
  assert(captured.values[0][0]==1000 && captured.values[0][1]==-1000 && captured.values[0][2]==-1000);
  assert(captured.interval_us==40000 && s_health.accel.count==0);
  e.values[0]=NAN;
  health_event(NULL,&e,NULL); assert(sent==1 && s_health.accel.count==0);
  e.count=17; health_event(NULL,&e,NULL); assert(sent==1);
  e.count=1; e.values[0]=72; health_event(NULL,&e,(void*)1);
  // Tizen accuracy 2 (good) leaves the adapter as Pebble quality 3.
  assert(sent==2 && captured.type==PBL_ADAPTER_SENSOR_HRM && captured.values[0][0]==72 &&
         captured.values[0][1]==3);
  e.values[0]=-3; health_event(NULL,&e,(void*)1);
  assert(sent==3 && captured.values[0][0]==-3 && captured.values[0][1]==-1);
  e.values[0]=NAN; health_event(NULL,&e,(void*)1); assert(sent==3);
  s_health.running[1]=false; e.values[0]=80; health_event(NULL,&e,(void*)1); assert(sent==3);
  assert(hrm_quality(72,2)==3);
  assert(hrm_quality(72,3)==4);
  assert(hrm_quality(72,1)==2);
  assert(hrm_quality(72,0)==0);
  assert(hrm_quality(-3,2)==-1);
  assert(hrm_quality(0,2)==0 && hrm_quality(-10,2)==0);
  e = (struct sensor_event){.timestamp=10000000,.count=3,.values={0,0,9.80665f}};
  s_health.accel_last_timestamp=0;
  health_event(NULL,&e,NULL);
  e.timestamp+=40000;
  health_event(NULL,&e,NULL);
  assert(s_health.accel.count==2);
  e.timestamp+=2000000;
  health_event(NULL,&e,NULL);
  assert(sent==4 && captured.count==2 && captured.timestamp_us==10000000);
  assert(s_health.accel.count==1 && s_health.accel.timestamp_us==e.timestamp);
  health_event(NULL,&e,NULL);
  e.timestamp-=40000;
  health_event(NULL,&e,NULL);
  assert(sent==4 && s_health.accel.count==1);
  s_health.get=fake_get; s_health.create=fake_create; s_health.set_cb=fake_cb;
  s_health.option=fake_option; s_health.batch=fake_batch;
  s_health.start=fake_start; s_health.stop=fake_stop; s_health.destroy=fake_destroy;
  s_health.listener[0]=(void *)4;
  uint64_t now=trace_clock();
  s_health.accel_last_event=now;
  recover_health(now+29);
  assert(stops==0 && destroys==0 && creates==0);
  recover_health(now+30);
  assert(stops==1 && destroys==1 && creates==1 && starts==1);
  assert(s_health.running[0] && s_health.samples==125 && s_health.interval_ms==40);
  assert(s_health.accel.count==0 && s_health.accel_last_timestamp==0);
  assert(batch_ms==5000 && callback_ms==40);
  recover_health(trace_clock());
  assert(starts==1);
  start_result=-EIO;
  recover_health(s_health.accel_last_event+30);
  assert(!s_health.running[0] && starts==2);
  recover_health(trace_clock());
  assert(starts==2);
  start_result=0;
  recover_health(s_health.accel_last_event+30);
  assert(s_health.running[0] && starts==3 && stops==2 && destroys==3);
  configure_health(0,0,40);
  recover_health(s_health.accel_last_event+90);
  assert(!s_health.running[0] && starts==3 && stops==3);
  s_health.set_events_cb=fake_events_cb; s_health.set_interval=fake_interval;
  configure_health(0,125,40);
  assert(s_health.running[0] && starts==4 && callback_ms==40 && batch_ms==5000);
  struct sensor_event bulk[125];
  for (unsigned i=0; i<125; i++) {
    bulk[i]=(struct sensor_event){.accuracy=2,.timestamp=20000000+i*40000,.count=3,
                                 .values={i*0.00980665f,0,9.80665f}};
  }
  unsigned before_bulk=sent;
  health_events(NULL,NULL,125,NULL); health_events(NULL,bulk,0,NULL);
  health_events(NULL,bulk,-1,NULL);
  assert(sent==before_bulk);
  health_events(NULL,bulk,125,NULL);
  assert(sent==before_bulk+1 && captured.count==125 && captured.timestamp_us==20000000);
  assert(captured.values[0][0]==0 && captured.values[124][0]==124);
  assert(s_health.accel.count==0 && s_health.max_callback_events==125);
  configure_health(0,0,40);
  s_health.set_interval=NULL;
  configure_health(0,125,40);
  assert(s_health.running[0] && starts==5 && callback_ms==40);
  configure_health(0,0,40);
  char trace_name[] = "/tmp/pebble-sensor-trace-XXXXXX";
  int trace_file = mkstemp(trace_name);
  assert(trace_file >= 0);
  close(trace_file);
  assert(trace_start(trace_name,301)==-EINVAL);
  assert(trace_start(trace_name,10)==0);
  trace_packet(&captured);
  assert(s_trace.bytes==sizeof(captured));
  s_trace.deadline=0;
  trace_packet(&captured);
  assert(s_trace.fd==-1 && s_trace.bytes==sizeof(captured));
  FILE *trace=fopen(trace_name,"rb");
  struct pbl_adapter_sensor replay;
  assert(trace && fread(&replay,sizeof(replay),1,trace)==1);
  assert(memcmp(&replay,&captured,sizeof(replay))==0 && fgetc(trace)==EOF);
  fclose(trace);
  assert(trace_start(trace_name,10)==0);
  s_trace.bytes=TRACE_MAX_BYTES;
  trace_packet(&captured);
  assert(s_trace.fd==-1);
  unlink(trace_name);
  // Screen-off rate: events faster than requested are decimated, and the measured hub rate
  // (not the request) is published for the FIFO helper.
  s_health.set_interval=fake_interval;
  configure_health(0,125,200);
  assert(s_health.running[0] && callback_ms==200 && s_health.interval_ms==200);
  assert(batch_ms==5000 && published_interval()==40);
  e = (struct sensor_event){.timestamp=50000000,.count=3,.values={0,0,9.80665f}};
  for (unsigned i=0;i<125;i++) { health_event(NULL,&e,NULL); e.timestamp+=40000; }
  assert(s_health.accel.count==25 && s_health.measured_interval_ms==40);
  assert(published_interval()==40);
  for (unsigned i=0;i<60;i++) { health_event(NULL,&e,NULL); e.timestamp+=200000; }
  assert(s_health.measured_interval_ms>=195 && s_health.measured_interval_ms<=200);
  assert(published_interval()==s_health.measured_interval_ms);
  unsigned before_rate=sent;
  configure_health(0,125,40);
  assert(sent==before_rate+1 && captured.interval_us==200000);  // partial batch kept
  assert(s_health.measured_interval_ms==0 && published_interval()==40);
  // Pedometer totals become deltas plus a heartbeat; stride time stays at 60-180 steps/min.
  s_steps.listener=(void *)4; s_steps.read=fake_read;
  unsigned before_steps=sent;
  poll_steps(1000,false);
  assert(sent==before_steps+1 && captured.type==PBL_ADAPTER_SENSOR_STEPS && captured.values[0][0]==0);
  poll_steps(1010,false);
  assert(sent==before_steps+1);
  fake_total=1030; poll_steps(1015,false);
  assert(sent==before_steps+2 && captured.values[0][0]==30 && captured.values[0][1]==15);
  assert(captured.timestamp_us==1015000000ull && captured.count==1);
  fake_total=1330; poll_steps(1030,false);
  assert(sent==before_steps+3 && captured.values[0][0]==300 && captured.values[0][1]==100);
  fake_total=1332; poll_steps(1630,false);
  assert(sent==before_steps+4 && captured.values[0][0]==2 && captured.values[0][1]==2);
  poll_steps(1650,false);
  assert(sent==before_steps+4);
  poll_steps(1695,false);
  assert(sent==before_steps+5 && captured.values[0][0]==0);
  fake_total=12; poll_steps(1710,false);  // midnight reset
  assert(sent==before_steps+6 && captured.values[0][0]==12);
  fake_read_result=-EIO; fake_total=99; poll_steps(1800,false);
  assert(sent==before_steps+6 && s_steps.total==12);
  fake_read_result=0;
  // Screen off: every 2 minutes.
  poll_steps(1900,true); unsigned off_sent=sent;
  poll_steps(1990,true); assert(sent==off_sent);
  poll_steps(2020,true); assert(sent==off_sent+1);
  // Pebble's activity request alone drops to 5 Hz with the screen off, never with HR on.
  s_health.requested_samples=125; s_health.requested_interval_ms=40;
  update_accel_rate(true,false);
  assert(s_health.interval_ms==SCREEN_OFF_INTERVAL_MS && callback_ms==SCREEN_OFF_INTERVAL_MS);
  update_accel_rate(false,false);
  assert(s_health.interval_ms==40 && callback_ms==40);
  s_health.running[1]=true;
  update_accel_rate(true,false);
  assert(s_health.interval_ms==40);
  s_health.running[1]=false;
  s_health.requested_samples=25;
  update_accel_rate(true,false);
  assert(s_health.interval_ms==40 && s_health.samples==25);
  s_steps.have_total=false; s_health.requested_samples=125;
  update_accel_rate(true,false);
  assert(s_health.interval_ms==40 && s_health.samples==125);
  configure_health(0,0,40);

  // Handshake and state messages.
  s_has.haptic = true; s_has.brightness = true; s_has.light = true; s_has.wrist = true;
  s_has.steps = true; s_has.suspend = false;
  send_caps(77);
  struct pbl_adapter_caps caps;
  assert(other_size == sizeof(caps));
  memcpy(&caps, other, sizeof(caps));
  assert(caps.hdr.type == PBL_ADAPTER_CAPS && caps.hdr.major == PBL_ADAPTER_MAJOR);
  assert(caps.ack_seq == 77);
  assert(caps.caps & PBL_ADAPTER_CAP_VIBE && caps.caps & PBL_ADAPTER_CAP_ALS &&
         caps.caps & PBL_ADAPTER_CAP_WRIST_GESTURE && caps.caps & PBL_ADAPTER_CAP_STEPS &&
         caps.caps & PBL_ADAPTER_CAP_ACCEL && caps.caps & PBL_ADAPTER_CAP_DISPLAY_POWER);
  assert(!(caps.caps & PBL_ADAPTER_CAP_SUSPEND) && !(caps.caps & PBL_ADAPTER_CAP_AMBIENT_LOWPOWER));
  assert(!strcmp(caps.adapter_name, ADAPTER_NAME) && !strcmp(caps.adapter_version, ADAPTER_VERSION));
  assert(caps.accel_max_samples == 125 && caps.vibe_max_duration_ms == 5000);
  s_has.suspend = true;
  assert(adapter_caps() & PBL_ADAPTER_CAP_SUSPEND);
  s_has.light = false;
  assert(!(adapter_caps() & PBL_ADAPTER_CAP_ALS));

  // STATE reports the applied mode, even when the OS has requested another one.
  light_lux = 321; light_valid = true;
  s_want.mode = PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK; s_want.seq = 9; s_want.applied_seq = 9;
  s_display_actual = PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK;
  char policy_name[] = "/tmp/pebble-fifo-policy-XXXXXX";
  int policy_file = mkstemp(policy_name);
  assert(policy_file >= 0);
  close(policy_file);
  snprintf(s_policy_path, sizeof(s_policy_path), "%s", policy_name);
  publish_state();
  struct pbl_adapter_state state;
  assert(other_size == sizeof(state));
  memcpy(&state, other, sizeof(state));
  assert(state.hdr.type == PBL_ADAPTER_STATE && state.ack_seq == 9 && state.lux == 321 &&
         state.lux_valid && state.display_actual == PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK);
  s_want.mode = PBL_ADAPTER_DISPLAY_OFF;
  s_display_actual = PBL_ADAPTER_DISPLAY_OFF;
  publish_state();
  memcpy(&state, other, sizeof(state));
  assert(state.display_actual == PBL_ADAPTER_DISPLAY_OFF);
  s_display_actual = PBL_ADAPTER_DISPLAY_ON;
  publish_state();
  memcpy(&state, other, sizeof(state));
  assert(state.display_actual == PBL_ADAPTER_DISPLAY_ON);
  assert(state.ack_seq == 9);

  // The FIFO helper's policy line: version 2, no OS files involved.
  FILE *policy = fopen(policy_name, "r");
  unsigned version, ambient, samples_seen, interval_seen, hrm_seen, alive, frozen;
  long long stamp;
  assert(policy && fscanf(policy, "%u %lld %u %u %u %u %u %u", &version, &stamp, &ambient,
                          &samples_seen, &interval_seen, &hrm_seen, &alive, &frozen) == 8);
  fclose(policy);
  assert(version == 2 && ambient == 0 && frozen == s_frozen_events);
  assert(alive == 1);  // the captured send succeeded, so an OS is listening
  unlink(policy_name);

  // A frozen accelerometer raises FAULT for the OS and a counter for the helper.
  struct pbl_adapter_sensor flat = {.type = PBL_ADAPTER_SENSOR_ACCEL, .count = 125,
                                    .interval_us = 40000};
  unsigned events_before = s_frozen_events;
  check_frozen(&flat);
  assert(s_frozen_events == events_before);
  check_frozen(&flat);
  assert(s_frozen_events == events_before + 1);
  struct pbl_adapter_fault fault;
  assert(other_size == sizeof(fault));
  memcpy(&fault, other, sizeof(fault));
  assert(fault.hdr.type == PBL_ADAPTER_FAULT && fault.component == PBL_ADAPTER_COMPONENT_ACCEL &&
         fault.state == PBL_ADAPTER_FAULT_RECOVERING);
  flat.values[3][0] = 5;
  check_frozen(&flat);
  memcpy(&fault, other, sizeof(fault));
  assert(fault.state == PBL_ADAPTER_FAULT_OK);

  // The same sensor_event timestamp is already in microseconds for every sensor kind.
  s_wrist_timestamp = 5000000000ull;
  send_gesture(PBL_ADAPTER_GESTURE_WRIST_UP);
  struct pbl_adapter_gesture gesture;
  assert(other_size == sizeof(gesture));
  memcpy(&gesture, other, sizeof(gesture));
  assert(gesture.hdr.type == PBL_ADAPTER_GESTURE && gesture.kind == PBL_ADAPTER_GESTURE_WRIST_UP &&
         gesture.timestamp_us == 5000000000ull);
  send_gesture(PBL_ADAPTER_GESTURE_WRIST_DOWN);
  memcpy(&gesture, other, sizeof(gesture));
  assert(gesture.kind == PBL_ADAPTER_GESTURE_WRIST_DOWN &&
         gesture.timestamp_us == s_wrist_timestamp);
  uint64_t before_gesture = pbl_adapter_boottime_us();
  send_gesture(PBL_ADAPTER_GESTURE_EXTERNAL_WAKE);
  memcpy(&gesture, other, sizeof(gesture));
  assert(gesture.timestamp_us >= before_gesture &&
         gesture.timestamp_us <= pbl_adapter_boottime_us());

  // Failed vendor operations keep the previous mode and acknowledgement until a retry succeeds.
  const struct display_power_ops power_ops = {
      .wake = fake_wake, .request_lock = fake_display_lock,
      .release_lock = fake_display_unlock, .change_state = fake_display_state,
  };
  s_display_actual = PBL_ADAPTER_DISPLAY_OFF;
  s_want.seq = 80;
  for (power_failure = 1; power_failure <= 2; power_failure++) {
    assert(apply_display_power(PBL_ADAPTER_DISPLAY_ON, false, &power_ops) == -EIO);
    assert(s_display_actual == PBL_ADAPTER_DISPLAY_OFF && s_want.applied_seq == 9);
    assert(s_display_failed && !s_display_lock);
  }
  power_failure = 0;
  assert(apply_display_power(PBL_ADAPTER_DISPLAY_ON, false, &power_ops) == 0);
  assert(s_display_actual == PBL_ADAPTER_DISPLAY_ON && s_want.applied_seq == 80);
  assert(!s_display_failed && s_display_lock);
  s_want.seq = 81;
  for (power_failure = 3; power_failure <= 4; power_failure++) {
    assert(apply_display_power(PBL_ADAPTER_DISPLAY_OFF, false, &power_ops) == -EIO);
    assert(s_display_actual == PBL_ADAPTER_DISPLAY_ON && s_want.applied_seq == 80);
    assert(s_display_failed);
  }
  power_failure = 0;
  assert(apply_display_power(PBL_ADAPTER_DISPLAY_OFF, false, &power_ops) == 0);
  assert(s_display_actual == PBL_ADAPTER_DISPLAY_OFF && s_want.applied_seq == 81);
  assert(!s_display_failed && !s_display_lock);
  puts("ADAPTER_DISPLAY_FAILURE_ACK_RETRY_OK");

  // A refused send marks the OS as gone; a successful one marks it alive.
  puts("ADAPTER_V2_CAPS_STATE_POLICY_FAULT_GESTURE_OK");
  puts("SENSOR_SCREEN_OFF_RATE_DECIMATION_MEASURED_INTERVAL_OK");
  puts("SENSOR_HUB_STEPS_DELTA_HEARTBEAT_STRIDE_RESET_OK");
  puts("SENSOR_ABI_ACCEL_UNITS_BATCHING_INVALID_VALUES_HRM_OFF_SWITCH_OK");
  puts("SENSOR_TRACE_EXACT_PACKETS_DEADLINE_SIZE_LIMIT_OK");
  puts("SENSOR_GAPS_DUPLICATES_STALL_RECOVERY_RETRY_DISABLED_OK");
  puts("SENSOR_BULK_ARRAY_STRIDE_ALL_SAMPLES_INTERVAL_ORDER_FALLBACK_OK");
  return 0;
}
