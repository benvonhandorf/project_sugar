
from mqtt_configuration import MqttConfiguration
from mqtt_connection import MqttConnection
import time
import logging
import signal
import os

logger = None
mqtt_connection = None

enable_service_name = 'camera_stream'
disable_service_name = 'camera_stream'

def on_control_message(topic, payload):
    logger.info(f'{topic}: {payload}')

    if payload == "enable_camera":
        logger.info("Enabling camera")
        publish_status('enable_camera')

        os.system(f'sudo systemctl start {enable_service_name}')
    elif payload == "disable_camera":
        logger.info("Disabling camera")
        publish_status('disable_camera')

        os.system(f'sudo systemctl stop {disable_service_name}')
    elif payload == "shutdown":
        logger.info("Shutting down")
        publish_status('shutdown')

        time.sleep(10)

        os.system(f'sudo shutdown -h now')

def publish_status(status_value):
    mqtt_connection.publish('status', status_value)

if __name__ == "__main__":
    logger = logging.getLogger()

    logger.setLevel(logging.DEBUG)
    stream_handler = logging.StreamHandler()
    stream_handler.setLevel(logging.DEBUG)

    formatter = logging.Formatter(fmt = '%(asctime)s %(name)s %(levelname)-8s %(message)s',
                                datefmt='%Y-%m-%d %H:%M:%S')
    stream_handler.setFormatter(formatter)

    logger.addHandler(stream_handler)

    hostname = os.uname()[1]

    if hostname == 'razorcrest':
        root_topic = '/cameras/razorcrest/camera0/'
        #Razorcrest runs the camera monitor locally, which depends on the camera_stream so this
        #service will start both.
        enable_service_name = 'camera_monitor'
    elif hostname == 'picam01' or hostname == 'primemover':
        root_topic = '/cameras/picam01/camera0/'
    else:
        logger.warning(f'Unknown hostname {hostname}.')
        exit(-1)

    mqtt_configuration = MqttConfiguration('littlerascal', 'camera', '8vSZa&#v7p1N', root_topic, 'camera_controller')

    mqtt_connection = MqttConnection(mqtt_configuration)

    mqtt_connection.subscribe('control', on_control_message)

    mqtt_connection.connect()

    signal.signal(signal.SIGINT, signal.SIG_DFL)
    signal.pause()