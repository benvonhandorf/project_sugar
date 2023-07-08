import cv2
from multiprocessing import Process, Queue
import logging
from frame_buffer import FrameBuffer
from stream_configuration import StreamConfiguration
from datetime import datetime
import common
from time import perf_counter

class SnapshotWriter(Process):
    def __init__(self, snapshot_control_queue: Queue, capture_complete_queue: Queue, frame_buffer: FrameBuffer, stream_configuration : StreamConfiguration):
        Process.__init__(self)

        self.queue = snapshot_control_queue
        self.capture_complete_queue = capture_complete_queue
        self.frame_buffer = frame_buffer
        self.stream_configuration = stream_configuration

    def perform_snapshot(self, timestamp):
        try:
            timestamp_string = datetime.fromtimestamp(timestamp/1000).isoformat()

            filename = f'{self.stream_configuration.directory}/{self.stream_configuration.base_filename}-{timestamp_string}.png'

            self.logger.info(f'Writing to {filename}')

            frame = self.frame_buffer.snapshot()

            cv2.imwrite(filename, frame)

            capture_complete = {'video': False, 'filename': filename}

            self.capture_complete_queue.put(capture_complete)

            self.logger.info(f'Capture complete: {capture_complete}')
        except Exception as e:
            self.logger.error(f'Error writing snapshot: {e}')

    def process_command(self, command):
        if command is not None:
            self.logger.debug(f'Received command: {command}')

            if command['value']:
                self.logger.debug(f'Taking snapshot')

                self.perform_snapshot(command['timestamp'])
        
    def run(self):
        self.logger = common.get_logger('SnapshotWriter')

        self.logger.info(f'Running')

        while True:
            try:
                command = self.queue.get()

                self.logger.info(f'Processing command: {command}')

                self.process_command(command)
            except Exception as e:
                self.logger.error(f'Error: {e}')

                    

            
                


        

