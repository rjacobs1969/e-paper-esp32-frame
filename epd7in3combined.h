/*****************************************************************************
* | File        :   epd7in3combined.h
* | Function    :   7.3inch e-Paper Spectra 6 (7IN3E) driver
******************************************************************************/

#ifndef __EPD_7IN3_H__
#define __EPD_7IN3_H__

#include "epdif.h"

// Display resolution
#define EPD_WIDTH       800
#define EPD_HEIGHT      480

#define UWORD   unsigned int
#define UBYTE   unsigned char
#define UDOUBLE  unsigned long

/**********************************
Color Index for EPD_7IN3E (Spectra 6)
**********************************/
#define EPD_7IN3E_BLACK   0x0   /// 000
#define EPD_7IN3E_WHITE   0x1   /// 001
#define EPD_7IN3E_YELLOW  0x2   /// 010
#define EPD_7IN3E_RED     0x3   /// 011
#define EPD_7IN3E_BLUE    0x5   /// 101
#define EPD_7IN3E_GREEN   0x6   /// 110

// Convenience aliases
#define EPD_WHITE EPD_7IN3E_WHITE
#define EPD_RED   EPD_7IN3E_RED



class Epd : EpdIf {
public:
    Epd();
    ~Epd();
    int  Init(void);
	void EPD_7IN3F_BusyHigh(void);
	void TurnOnDisplay(void);
    void Reset(void);
    void EPD_7IN3F_Display(const UBYTE *image);
    void EPD_7IN3F_Display_part(const UBYTE *image, UWORD xstart, UWORD ystart,
                                 UWORD image_width, UWORD image_heigh);
    void EPD_7IN3F_Draw_Blank(UWORD rows, UWORD cols, UBYTE color);
    // void EPD_7IN3F_Show7Block(void);
    void SendCommand(unsigned char command);
    void SendData(unsigned char data);
    void Sleep(void);
    void Clear(UBYTE color);

private:
    unsigned int reset_pin;
    unsigned int dc_pin;
    unsigned int cs_pin;
    unsigned int busy_pin;
    unsigned long width;
    unsigned long height;
};

#endif /* __EPD_7IN3_H__ */

/* END OF FILE */
