import cv2
from time import sleep

def main():
    vcap = cv2.VideoCapture("rtsp://littlerascal:8554/picam2")
    vcap2 = cv2.VideoCapture("rtsp://littlerascal:8554/polecat")

    cv2.startWindowThread()
    cv2.namedWindow('picam2')
    cv2.namedWindow('polecat')

    while True:
        if vcap.isOpened():
            ret, frame = vcap.read()
            cv2.imshow('picam2', frame)
        else:
            print('picam2 is offline')
        if vcap2.isOpened():
            ret, frame = vcap2.read()
            cv2.imshow('polecat', frame)
        else:
            print('polecat is offline')

        sleep(5)

    print('Complete')

if __name__ == '__main__':
    print('Starting')
    main()