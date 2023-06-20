from multiprocessing import Process, Queue
from frame_buffer import FrameBuffer
from stream_configuration import StreamConfiguration
from datetime import datetime
import common
from time import perf_counter

class FileMover(Process):
    def __init__(self, capture_complete_queue: Queue, move_complete_queue: Queue):
        Process.__init__(self)

        self.capture_complete_queue = capture_complete_queue

    def process_capture_complete(self, capture_complete):
        pass

    def run(self):
        self.logger = common.get_logger('FileMover')

        self.logger.info(f'Running')

        while True:
            capture_complete = self.capture_complete_queue.get()

            self.logger.info(f'File Move Request: {capture_complete}')

            self.process_capture_complete(capture_complete)