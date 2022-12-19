#include "settings.h"

#include "painlessMesh.h"

#if defined(CENTRAL_MODULE)
// === CENTRAL MODULE SECTION SOURCE CODE ===
#include "MQUnifiedsensor.h"

#include <memory>
#include <cstring>

#define BOARD "ESP32"
#define V_RES 3.3
#define ADC_BIT 12

#define RAT_MQ131_CA 15

//TODO: Add BME688
MQUnifiedsensor MQ131(BOARD, V_RES, ADC_BIT, MQ131_PIN, "MQ-131");

Scheduler mainscheduler; // task scheduler

painlessMesh mesh;

std::shared_ptr<Task> send_msg_task;

SerialTransfer transfer;

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

void parseMsg(const String& msg) {
  char* temp = std::strtok((char*)msg.c_str(), "/");
  static String val[4];
  i = 0;
  while (temp) {
    val[i] = temp;
    temp = std::strtok(nullptr, "/");  
    i++;
  }
  sensData.sensor_id = val[0].toInt();
  sensData.humid = val[1].toFloat();
  sensData.moisture = val[2].toFloat();
  sensData.temp = val[3].toFloat();
  delete temp;
}

void receivedCallback(uint32_t& from, const String& msg ) {
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

  // TODO: send to MQTT
}

void setup() {
  Serial.begin(115200);

  send_msg_task = std::make_shared<Task>(TASK_SECOND * 5, TASK_FOREVER, sendMsgRoutine);

  MQ131.setRegressionMethod(1);
  MQ131.setA(23.943);
  MQ131.setB(-1.11);
  MQ131.init();


  Serial.print("Calibrating gas sensor, please wait.");
  float calcR0_131, calcR0_2; // declare in setup because one time only
  calcR0_131 = 0;
  calcR0_2 = 0;
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
  mesh.onReceive(receivedCallback);

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
#include "yl3869.h"
#include "DHTesp.h"

#include <memory>

#define uS_TO_S 1E+6
#define uS_TO_HOURS 3.6E+9

Scheduler mainscheduler;  // main task scheduler

painlessMesh mesh;

std::shared_ptr<Task> send_msg_task;

// improve later: get sensor count from predefined pin arrays
int DHT_SENSOR_COUNT = sizeof(DHT_SENSOR_PINS)/sizeof(DHT_SENSOR_PINS[0]);
int MOIST_SENSOR_COUNT = sizeof(MOIST_SENSOR_PINS)/sizeof(MOIST_SENSOR_PINS[0]);

DHTesp *dht[DHT_SENSOR_COUNT];
YL3869 *yl3869[MOIST_SENSOR_COUNT];

uint8_t i;

for(i = 0; i < DHT_SENSOR_COUNT; i++) {
  dht[i] = &DHTesp(DHT_SENSOR_PINS[i], models::DHT22); // add define DHT models later
}

for(i = 0; i < MOIST_SENSOR_COUNT; i++) {
  yl3869[i] = &YL3869(MOIST_SENSOR_PINS[i]);
}

// = { DHTesp(12, models::DHT22), DHTesp(13, models::DHT22)};
// = { YL3869(32), YL3869(33) };

float humid[DHT_SENSOR_COUNT], temp[DHT_SENSOR_COUNT];
float moisture[MOIST_SENSOR_COUNT];
uint8_t SENSOR_COUNT = DHT_SENSOR_COUNT > MOIST_SENSOR_COUNT ? DHT_SENSOR_COUNT : MOIST_SENSOR_COUNT;
uint8_t sensor_id[SENSOR_COUNT];

for (i = 1; i <= SENSOR_COUNT; i++) {
  sensor_id[i] = i;
}

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
  send_msg_task->set(TASK_MILLISECOND * 100, TASK_FOREVER, sendMsgRoutine);
}

void sendMsgRoutine() {

  if (i == SENSOR_COUNT) {
    mesh.sendSingle(1, "S");  // confirmation message
    Serial.println("SEND: S");
    send_msg_task->delay(TASK_SECOND * 8);
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
    esp_sleep_enable_timer_wakeup(20 * uS_TO_S);
    esp_deep_sleep_start();
  }
}

void setup() {
  Serial.begin(115200);

  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);  // set before init() so that you can see startup messages
  
  /*
  for (i = 0; i < SENSOR_COUNT; i++) {
    dht[i].begin();
    yl3869[i].init();
  }
  */

  send_msg_task = std::make_shared<Task>(TASK_MILLISECOND * 1, TASK_ONCE, readSensorRoutine);
  
  mesh.init(MESH_PREFIX, MESH_PASSWORD, mainscheduler, MESH_PORT, WIFI_AP_STA, NODE_NUMBER);  // node number can be changed from settings.h
  mesh.onReceive(receivedCallback);
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