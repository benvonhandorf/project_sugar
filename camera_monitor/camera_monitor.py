import cv2
import argparse
import logging
from time import perf_counter
import frame_buffer
from stream_configuration import StreamConfiguration
from stream_reader import StreamReader
from stream_writer import StreamWriter
from snapshot_writer import SnapshotWriter
from file_mover import FileMover, FileMoverConfig
from multiprocessing import Queue, log_to_stderr
from controller import Controller
from mqtt_monitor import MqttConfiguration, MqttMonitor
import common
import os
import sys
import json

if __name__ == '__main__':
    logger = common.get_logger()

    logger.info('Starting')

    hostname = os.uname()[1]

    config_file = sys.argv[1] or '../camera_config.json'

    logger.info(f'Loading configuration from {config_file}')

    with open(config_file, 'r') as config_file:
        CONFIG = json.load(config_file)

    camera_host = CONFIG.get('camera_host') or os.uname()[1]
    camera_id = CONFIG['camera_id']

    mqtt_host = CONFIG['mqtt_host']
    mqtt_port = CONFIG['mqtt_port']
    mqtt_username = CONFIG['mqtt_username']
    mqtt_password = CONFIG['mqtt_password']

    root_topic = f'cameras/{camera_host}/{camera_id}/'

    camera_id = 'camera0'

    if hostname == 'razorcrest':
        stream_configuration = StreamConfiguration(f'rtsp://{hostname}:8554/{camera_id}', 'data', 'razorcrest', 30, 1280, 720)
    else :
        stream_configuration = StreamConfiguration(f'rtsp://{hostname}:8554/{camera_id}', 'data', 'razorcrest', 30, 1280, 720)
        stream_configuration = StreamConfiguration('rtsp://picam01:8554/camera0', 'data', 'picam01', 30, 800, 600)
        
    client_id = f'{hostname}_{camera_id}'
    topic = f'cameras/{hostname}/{camera_id}/'
    feeder_id = 'feeder01'

    if not os.path.exists(stream_configuration.directory):
        logger.info(f'Creating directory {stream_configuration.directory}')
        os.makedirs(stream_configuration.directory)

    file_mover_config = FileMoverConfig('littlerascal', 'camera_files', f'storage/{feeder_id}/')

    frame_buffer_seconds = 5
    frame_buffer_count = stream_configuration.framerate * frame_buffer_seconds

    logger.info(f'Frame Buffer for {frame_buffer_count} frames.')

    frame_buffer_manager = frame_buffer.FrameBufferManager()
    frame_buffer_manager.start()

    frame_buffer = frame_buffer_manager.FrameBuffer(frame_buffer_count)

    control_queue = Queue()
    snapshot_queue = Queue()
    capture_complete_queue = Queue()
    file_moved_queue = Queue()

    stream_writer = StreamWriter(control_queue, capture_complete_queue, frame_buffer, stream_configuration)
    # stream_writer.daemon = True
    stream_writer.start()

    stream_reader = StreamReader(stream_configuration, frame_buffer)
    # stream_reader.daemon = True
    stream_reader.start()

    snapshot_writer = SnapshotWriter(snapshot_queue, capture_complete_queue, frame_buffer, stream_configuration)
    snapshot_writer.start()

    file_mover = FileMover(file_mover_config, capture_complete_queue, file_moved_queue)
    file_mover.start()

    controller = Controller(control_queue, snapshot_queue)

    mqtt_configuration = MqttConfiguration('littlerascal', 'camera', '8vSZa&#v7p1N', topic, client_id)

    mqtt_monitor = MqttMonitor(mqtt_configuration, controller, file_moved_queue)
    mqtt_monitor.start()

    while True:
        pass