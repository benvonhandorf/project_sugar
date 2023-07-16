from multiprocessing import Process, Queue
from collections import deque
from multiprocessing.managers import BaseManager

class FrameBufferManager(BaseManager):
    pass

class FrameBuffer(object):
    def __init__(self, frame_count, snapshot_offset_frames):
        self.deque = deque(maxlen=frame_count)
        self.latest_frame = None
        self.snapshot_offset_frames = snapshot_offset_frames

    def len(self):
        return len(self.deque)
    
    def append(self, x):
        self.deque.append(x)
        self.latest_frame = x

    def pop(self):
        if self.deque:
            return self.deque.popleft()
        else:
            return None

    def snapshot(self):
        if len(self.deque) > self.snapshot_offset_frames:
            return self.deque[-self.snapshot_offset_frames]
        else:
            return self.latest_frame

FrameBufferManager.register('FrameBuffer', FrameBuffer)