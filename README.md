Project done on a Nucleo-F767ZI board  
SPI configurations for the display:  
<img width="667" height="686" alt="image" src="https://github.com/user-attachments/assets/00ed83f6-fc52-4e82-8382-10e1845d2f3b" />

SAI configurations for the microphone:  
<img width="678" height="838" alt="image" src="https://github.com/user-attachments/assets/eece8e8b-7e60-459b-9e43-32f25c48bade" />
<img width="834" height="655" alt="image" src="https://github.com/user-attachments/assets/be871769-196a-484f-9566-797af8f12464" />

Wiring for Display (https://www.waveshare.com/wiki/2inch_LCD_Module?amazon#Software_description):  
| LCD          | STM32 |
|--------------|-------|
|VCC           |  3.3V |
|GND           |  GND  |
|DIN           |  MOSI |
|CLK           |  SCK  |
|CS            |  IO   |
|DC            |  IO   | 
|RST           |  IO   | 
|BL (optional  |  TIM  |   
------------------------  
Wiring for INMP441 microphone (https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf):  
<img width="684" height="415" alt="image" src="https://github.com/user-attachments/assets/c859a881-3491-48c1-b72b-4c060cc43361" />

Optional UART configuration to see Raw microphone/FFT data through a terminal (see more in main.c):  
<img width="855" height="755" alt="image" src="https://github.com/user-attachments/assets/088c5aa5-25ff-4cca-9b45-1eb595828729" />


Credits to Golinskiy Konstantin for the ST7789 Display drivers
