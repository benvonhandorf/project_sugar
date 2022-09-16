import cv2
from time import sleep

def main():
    vcap = cv2.VideoCapture("rtsp://littlerascal:8554/polecat")

    frames = 0

    while vcap.isOpened():
        ret, frame = vcap.read()
        frames+= 1

        if frames % 100 == 0:
            print(frames)        

    print('Complete')

if __name__ == '__main__':
    print('Starting')
    main()