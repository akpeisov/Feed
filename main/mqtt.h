//mqtt.h

void initMQTT();
void mqttPublish(char* topic, char* data);
void mqttPublishF(char* topic, float fdata);