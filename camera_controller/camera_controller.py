
from mqtt_configuration import MqttConfiguration
from mqtt_connection import MqttConnection
import time
import logging
import signal
import os
import json
import sys

logger = None
mqtt_connection = None

services = ['camera_stream', 'camera_monitor']

def on_control_message(topic, payload):
    logger.info(f'{topic}: {payload}')

    if payload == "enable_camera":
        logger.info("Enabling camera")
        publish_status('enable_camera')

        for service in services:
            try:
                os.system(f'sudo systemctl start {service}')
            except:
                pass
    elif payload == "disable_camera":
        logger.info("Disabling camera")
        publish_status('disable_camera')

        for service in services:
            try:
                os.system(f'sudo systemctl stop {service}')
            except:
                pass

    elif payload == "restart_camera":
        logger.info("Restart camera")
        logger.info("Disabling camera")
        publish_status('disable_camera')

        for service in services:
            try:
                os.system(f'sudo systemctl stop {service}')
            except:
                pass

        time.sleep(5)

        logger.info("Enabling camera")
        publish_status('enable_camera')

        for service in services:
            try:
                os.system(f'sudo systemctl start {service}')
            except:
                pass

    elif payload == "shutdown":
        logger.info("Shutting down")
        publish_status('shutdown')

        time.sleep(5)

        os.system(f'sudo shutdown -h now')

    elif payload == "reboot":
        logger.info("Rebooting")
        publish_status('reboot')

        time.sleep(5)

        os.system(f'sudo shutdown -r now')

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

    config_file = sys.argv[1] or '../camera_config.json'

    hostname = os.uname()[1]

    logger.info(f'Loading configuration from {config_file}')

    with open(config_file, 'r') as config_file:
        CONFIG = json.load(config_file)

    camera_host = CONFIG.get('camera_host') or hostname
    camera_id = CONFIG['camera_id']

    mqtt_host = CONFIG['mqtt_host']
    mqtt_port = CONFIG['mqtt_port']
    mqtt_username = CONFIG['mqtt_username']
    mqtt_password = CONFIG['mqtt_password']

    root_topic = f'cameras/{camera_host}/{camera_id}/'

    client_id = f'{camera_host}_{camera_id}_controller'

    if hostname != camera_host:
        client_id += f'_{hostname}'

        services = [f'camera_monitor_{camera_host}']

    mqtt_configuration = MqttConfiguration(mqtt_host, mqtt_username, mqtt_password, root_topic, client_id)

    mqtt_connection = MqttConnection(mqtt_configuration)

    mqtt_connection.subscribe('control', on_control_message)

    mqtt_connection.connect()

    signal.signal(signal.SIGINT, signal.SIG_DFL)
    signal.pause()