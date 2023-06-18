from multiprocessing import Process
from stream_configuration import StreamConfiguration
from frame_buffer import FrameBuffer
import logging
from time import perf_counter
import cv2
import common

class StreamReader(Process):
    def __init__(self, stream_configuration:StreamConfiguration, frame_buffer: FrameBuffer):
        Process.__init__(self)

        self.frame_buffer = frame_buffer
        self.stream_configuration = stream_configuration

    def run(self):
        self.logger = common.get_logger('StreamReader')
        
        timestamp = perf_counter()
        frames = 0

        self.logger.info(f'Capture Source:{self.stream_configuration.source}')

        self.capture_source = cv2.VideoCapture(self.stream_configuration.source)

        new_ts = perf_counter()
        duration = new_ts - timestamp
        timestamp = new_ts
        self.logger.info(f'Capture Source created: {duration:.3f}s')
        
        while self.capture_source.isOpened():
            ret, frame = self.capture_source.read()

            if ret:
                self.frame_buffer.append(frame)

                frames += 1
                if frames == 500:
                    new_ts = perf_counter()
                    duration = new_ts - timestamp
                    timestamp = new_ts
                    fps = frames / duration
                    
                    self.logger.info(f'{fps:.3f} fps read.')

                    frames = 0
            else:
                self.logger.warn(f'Frame read failed')

        self.logger.warn(f'Capture source is closed.')



    
