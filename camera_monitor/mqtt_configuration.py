class MqttConfiguration:
    def __init__(self, broker, username, password, topic, client_id):
        self.broker = broker
        self.broker_port = 1883
        self.username = username
        self.password = password
        self.topic = topic
        self.client_id = client_id