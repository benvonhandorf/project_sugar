import cv2
import argparse
import logging
from time import perf_counter
import frame_buffer
from stream_configuration import StreamConfiguration
from stream_reader import StreamReader
from stream_writer import StreamWriter
from multiprocessing import Queue, log_to_stderr
from controller import Controller
from mqtt_monitor import MqttConfiguration, MqttMonitor
import common
from multiprocessing_logging import install_mp_handler

if __name__ == '__main__':
    logger = common.get_logger()
    
    # stream_configuration = StreamConfiguration('rtsp://razorcrest:8554/camera0', 'razorcrest-video', 30, 1280, 720)
    # topics = ['/cameras/razorcrest/camera0/record']
    stream_configuration = StreamConfiguration('rtsp://picam01:8554/camera0', 'picam01', 30, 800, 600)
    topics = ['/cameras/picam01/camera0/record']

    frame_buffer_seconds = 5
    frame_buffer_count = stream_configuration.framerate * frame_buffer_seconds

    logger.info(f'Frame Buffer for {frame_buffer_count} frames.')

    frame_buffer_manager = frame_buffer.FrameBufferManager()
    frame_buffer_manager.start()

    frame_buffer = frame_buffer_manager.FrameBuffer(frame_buffer_count)

    control_queue = Queue()
    snapshot_queue = Queue()

    stream_writer = StreamWriter(control_queue, frame_buffer, stream_configuration)
    # stream_writer.daemon = True
    stream_writer.start()

    stream_reader = StreamReader(stream_configuration, frame_buffer)
    # stream_reader.daemon = True
    stream_reader.start()

    controller = Controller(control_queue, snapshot_queue)

    mqtt_configuration = MqttConfiguration('littlerascal', 'camera', '8vSZa&#v7p1N', topics)

    mqtt_monitor = MqttMonitor(mqtt_configuration, controller)
    # mqtt_monitor.daemon = True
    mqtt_monitor.start()

    while True:
        pass