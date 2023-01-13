#include "settings.h"

#if defined(CENTRAL_MODULE)
// === CENTRAL MODULE SECTION SOURCE CODE ===
#include "MQUnifiedsensor.h"

#include <mavlink.h>
#include <memory>

#define BOARD "ESP32"
#define V_RES 3.3
#define ADC_BIT 12

#define RAT_MQ131_CA 15

//TODO: Add BME688
MQUnifiedsensor MQ131(BOARD, V_RES, ADC_BIT, MQ131_PIN, "MQ-131");

Scheduler mainscheduler; // task scheduler

painlessMesh mesh;

std::shared_ptr<Task> send_msg_task;

uint8_t i = 0;

struct {
  uint8_t sensor_id;
  uint8_t node_id;
  float humid;
  float moisture;
  float temp;
  float ozone;
  float gas;
} sensData;

struct Odom{
  float x;
  float y;
  float z;
  float vx;
  float vy;
  float vz;
}odom;

void req_data(){
  uint8_t sys_id = 255; // qgc id
  uint8_t comp_id = 2; // any?
  uint8_t tgt_sys = 1; // id of pxhawk = 1
  uint8_t tgt_comp = 1; // 0 broadcast, 1 work juga
  uint8_t req_stream_id = MAV_DATA_STREAM_ALL;
  uint16_t req_msg_rate = 0x01; // 1 times per second
  uint8_t start_stop = 1; // 1 start, 0 = stop

  mavlink_message_t msg;
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];

  mavlink_msg_request_data_stream_pack(sys_id, comp_id, &msg, tgt_sys, tgt_comp, req_stream_id, req_msg_rate, start_stop);
  uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);  // Send the message (.write sends as bytes)
 
  Serial1.write(buf, len); 
}

void read_data(){
  mavlink_message_t msg;
  mavlink_status_t status;
 
  while(Serial1.available())
  {
    uint8_t c = Serial1.read();
 
    //Get new message
    if(mavlink_parse_char(MAVLINK_COMM_0, c, &msg, &status))
    {
 
    //Handle new message from autopilot
      switch(msg.msgid)
      {
        case MAVLINK_MSG_ID_GPS_RAW_INT:
          mavlink_gps_raw_int_t packet;
          mavlink_msg_gps_raw_int_decode(&msg, &packet);
          
          Serial.print("\nGPS Fix: ");Serial.println(packet.fix_type);
          Serial.print("GPS Latitude: ");Serial.println(packet.lat);
          Serial.print("GPS Longitude: ");Serial.println(packet.lon);
          Serial.print("GPS Speed: ");Serial.println(packet.vel);
          Serial.print("Sats Visible: ");Serial.println(packet.satellites_visible);
          break;
        case MAVLINK_MSG_ID_ODOMETRY:
          
          break;
        
      }
    }
  }
}

void read_data_odom(mavlink_message_t& msg){
  mavlink_odometry_t odom_in;
  mavlink_msg_odometry_decode(&msg, &odom_in);

  odom.x = odom_in.x;
  odom.y = odom_in.y;
  odom.z = odom_in.z;
  odom.vx = odom_in.vx;
  odom.vy = odom_in.vy;
  odom.vz = odom_in.vz;

  Serial.print("\nX : ");Serial.println(odom.x);
  Serial.print("Y : ");Serial.println(odom.y);
  Serial.print("Z : ");Serial.println(odom.z);
  Serial.print("vX : ");Serial.println(odom.vx);
  Serial.print("vY : ");Serial.println(odom.vy);
  Serial.print("vZ : ");Serial.println(odom.vz);
}

void parseMsg(const String& msg) {
  static size_t pos[3];
  int l = msg.length();
  for(int i = 0; i < l; i++){
    if(msg[i] == '/'){
      pos[0] = i;
      break;
    }
  }
  // pos[0] = msg.find("/");
  sensData.sensor_id = msg.substring(0, pos[0]).toInt();
  for(int i = pos[0] + 1; i < l; i++){
    if(msg[i] == '/'){
      pos[1] = i;
      break;
    }
  }
  // pos[1] = msg.find("/", pos[0]+1);
  sensData.humid = msg.substring(pos[0]+1, pos[1]-pos[0]-1).toFloat();
  for(int i = pos[1] + 1; i < l; i++){
    if(msg[i] == '/'){
      pos[1] = i;
      break;
    }
  }
  // pos[2] = msg.find("/", pos[1]+1);
  sensData.moisture = msg.substring(pos[1]+1, pos[2]-pos[1]-1).toFloat();
  sensData.temp = msg.substring(pos[2]+1, l).toFloat();
}

void receivedCallback(uint32_t from, const String& msg ) {
  Serial.printf("RECV: %u msg=%s\n", from, msg.c_str());
  if (msg != "S") { // keep parsing when recieving
    parseMsg(msg);
    sensData.node_id = from;
  }
  else mesh.sendSingle(from, "A"); // acknowledge/ACK message
}

void sendMsgRoutine() {

  // TODO: add BME688 readings
  MQ131.update();

  sensData.ozone = MQ131.readSensorR0Rs();

  // TODO: send to back-end
}

void setup() {
  Serial.begin(115200);

  send_msg_task = std::make_shared<Task>(UPDATE_RATE, TASK_FOREVER, sendMsgRoutine);

  MQ131.setRegressionMethod(1);
  MQ131.setA(23.943);
  MQ131.setB(-1.11);
  MQ131.init();


  Serial.print("Calibrating gas sensor, please wait.");
  float calcR0_131 = 0;
  float calcR0_2 = 0;
  for(i = 1; i<=10; i ++) {
    MQ131.update();
    calcR0_131 += MQ131.calibrate(RAT_MQ131_CA);
    Serial.print(".");
    delay(1);
  }
  Serial.println(".");
  MQ131.setR0(calcR0_131/10);

  Serial.println("Gas sensor calibration complete");

  if(isinf(calcR0_131)) {Serial.println("Warning: Conection issue on MQ131, R0 is infinite (Open circuit detected) please check your wiring and supply");}
  if(calcR0_131 == 0){Serial.println("Warning: Conection issue found on MQ131, R0 is zero (Analog pin shorts to ground) please check your wiring and supply");}
  
  mesh.setDebugMsgTypes( ERROR | STARTUP );  // set before init() so that you can see startup messages

  mesh.init(MESH_PREFIX, MESH_PASSWORD, mainscheduler, MESH_PORT, WIFI_AP, 1); // Central node number will always be 1
  mesh.onReceive(&receivedCallback);

  mainscheduler.addTask(ConvTask::getFromShared(send_msg_task));
  send_msg_task->enable();
}

void loop() {
  // it will run the user scheduler as well
  mesh.update();
}

// === END OF CENTRAL MODULE SOURCE CODE SECTION ===
#else
// === NODE MODULE SECTION SOURCE CODE ===
constexpr uint8_t dht_sensor_count = sizeof(DHT_SENSOR_PINS)/sizeof(DHT_SENSOR_PINS[0]);
constexpr uint8_t moist_sensor_count = sizeof(MOIST_SENSOR_PINS)/sizeof(MOIST_SENSOR_PINS[0]);
constexpr uint8_t SENSOR_COUNT = floor( (dht_sensor_count + moist_sensor_count) / 2 );

#include <yl3869.h>
#include "DHTesp.h"

#include <memory>
#include <vector>

#define uS_TO_S 1E+6
#define uS_TO_HOURS 3.6E+9

Scheduler mainscheduler;  // main task scheduler

painlessMesh mesh;

std::shared_ptr<Task> send_msg_task;

std::shared_ptr<DHTesp> dht[SENSOR_COUNT];
std::shared_ptr<YL3869> yl3869[SENSOR_COUNT];

float humid[SENSOR_COUNT], temp[SENSOR_COUNT];
float moisture[SENSOR_COUNT];
uint8_t sensor_id[SENSOR_COUNT];

uint8_t i;

bool central_status;
bool queue_status = false;

String packMsg(uint8_t idx) {
  return (String(sensor_id[idx]) + "/" + String(humid[idx]) + "/" + String(temp[idx]) + "/" + String(moisture[idx]));
}

void readSensorRoutine() {
  for (i = 0; i < SENSOR_COUNT; i++) {
    humid[i] = 83;     //dht[i-1].getHumidity();
    temp[i] = 20;      //dht[i-1].getTemperature();  
    moisture[i] = 32;  //yl3869[i-1].read();
  }

  Serial.println("Sensor data read");

  if (mesh.isConnected(1)) Serial.println("Connected to central module. Sending");

  i = 0;
  send_msg_task->set(TASK_MILLISECOND, TASK_FOREVER, sendMsgRoutine);
}

void sendMsgRoutine() {

  if (i == SENSOR_COUNT) {
    mesh.sendSingle(1, "S");  // confirmation message
    Serial.println("SEND: S");
    send_msg_task->delay(REPEAT_SEND_RATE);
    return;
  }

  if (!mesh.isConnected(1)) {  // are we connected to central module?
    Serial.println("Not connected to central module. Queueing");
    queue_status = true;
    send_msg_task->cancel();
    return;
  }

  mesh.sendSingle(1, packMsg(i));
  Serial.println(packMsg(i).c_str());
  i++;
}

void newConnectionCallback(uint32_t nodeId) {
  
  if ((nodeId == 1) && (queue_status == true)) {
    Serial.println("Central module connection acquired, sending queued data");
    send_msg_task->restart();
    queue_status = false;
  }
}

// Needed for painless library
void receivedCallback(uint32_t from, const String& msg) {
  Serial.printf("RECV: %u msg=%s\n", from, msg.c_str());
  if (msg == "A") {
    Serial.println("Message sent. Change to Deep Sleep Mode");
    mesh.stop();
    esp_sleep_enable_timer_wakeup(UPDATE_RATE);
    esp_deep_sleep_start();
  }
}

void setup() {
  Serial.begin(115200);

  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);  // set before init() so that you can see startup messages
  
  for(i = 0; i < SENSOR_COUNT; i++) {
    sensor_id[i] = i+1;
    
    dht[i] = std::make_shared<DHTesp>(DHT_SENSOR_PINS[i], models::DHT22); // add define DHT models later
    dht[i]->begin();
    
    yl3869[i] = std::make_shared<YL3869>(MOIST_SENSOR_PINS[i]);
    yl3869[i]->init();
  }

  send_msg_task = std::make_shared<Task>(TASK_MILLISECOND, TASK_ONCE, readSensorRoutine);
  
  mesh.init(MESH_PREFIX, MESH_PASSWORD, mainscheduler, MESH_PORT, WIFI_AP_STA, NODE_NUMBER);  // node number can be changed from settings.h
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(newConnectionCallback);

  mainscheduler.addTask(ConvTask::getFromShared(send_msg_task));
  send_msg_task->enable();
}

void loop() {
  // it will run the user scheduler as well
  mesh.update();
}

// === END OF NODE MODULE SOURCE CODE SECTION ===
#endif