/*****************************************************************************
* | File      	:   AMOLED_1in8.c
* | Author      :   Waveshare Team
* | Function    :   AMOLED Interface Functions
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2025-03-20
* | Info        :   
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of theex Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
******************************************************************************/
#include "DEV_Config.h"
#include "AMOLED_1in8.h"

AMOLED_1IN8_ATTRIBUTES AMOLED_1IN8;

/********************************************************************************
function:	Sets the start position and size of the display area
parameter:
        qspi    ：  qspi structure
		Xstart 	:   X direction Start coordinates
		Ystart  :   Y direction Start coordinates
		Xend    :   X direction end coordinates
		Yend    :   Y direction end coordinates
********************************************************************************/
/******************************************************************************
function :  Wait until the PIO has actually clocked out everything queued
parameter:
Info:       dma_channel_is_busy() going false only means the DMA finished
            feeding the PIO TX FIFO. The state machine still has whatever is in
            the FIFO, plus the output shift register, left to clock onto the
            wire. Deasserting CS before that drains truncates the end of the
            transfer, so the last pixels are never written to the panel.

            Upstream drops CS immediately after the DMA wait at every call site.
            On a full-screen refresh the loss is a couple of pixels in the final
            corner and goes unnoticed; with many small partial refreshes, as a
            drawing app does, every push loses its tail and the result looks
            like speckled dead pixels along each stroke.
******************************************************************************/
static void AMOLED_1IN8_WaitShiftOut(void)
{
    while (!pio_sm_is_tx_fifo_empty(qspi.pio, qspi.sm)) {
        tight_loop_contents();
    }
    // The FIFO being empty still leaves the OSR mid-shift. At clkdiv 2.0 the
    // 4-wire program emits a byte every 8 cycles, so a few hundred nanoseconds
    // covers the shifter; 2us is a cheap margin against a 27us push.
    busy_wait_us(2);
}

void AMOLED_1IN8_SetWindows(uint32_t Xstart, uint32_t Ystart, uint32_t Xend, uint32_t Yend){
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x2a); 
    QSPI_DATA_Write(qspi, Xstart>>8);
    QSPI_DATA_Write(qspi, Xstart&0xff);
    QSPI_DATA_Write(qspi, (Xend-1)>>8);
    QSPI_DATA_Write(qspi, (Xend-1)&0xff);
    QSPI_Deselect(qspi); 
    
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x2b);
    QSPI_DATA_Write(qspi, Ystart>>8);
    QSPI_DATA_Write(qspi, Ystart&0xff);
    QSPI_DATA_Write(qspi, (Yend-1)>>8);
    QSPI_DATA_Write(qspi, (Yend-1)&0xff);
    QSPI_Deselect(qspi); 
    
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x2c);
    QSPI_Deselect(qspi); 
    WAIT_TIME();
}

/******************************************************************************
function :	Initialize the lcd register
parameter:
        qspi    ：  qspi structure
******************************************************************************/
static void AMOLED_1IN8_InitReg(){
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x11);
    // 150ms, not upstream 120ms. The SH8601 datasheet is explicit: "There is
    // needed 150ms after sleep out command, when there is changing from Sleep
    // in mode to sleep out mode". Upstream is UNDER spec on the one delay this
    // driver actually has to honour, while being wildly over-generous on the
    // reset pulses above. A marginal panel would show this as an unreliable
    // wake, the kind of intermittent fault nobody traces to a missing 30ms.
    sleep_ms(150);
    QSPI_Deselect(qspi);

    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x44);
    QSPI_DATA_Write(qspi, 0x01);
    QSPI_DATA_Write(qspi, 0xC5); 
    QSPI_Deselect(qspi);
    
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x35);
    QSPI_DATA_Write(qspi, 0x00);
    QSPI_Deselect(qspi);

    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x3A);
    QSPI_DATA_Write(qspi, 0x55);  
    QSPI_Deselect(qspi);
    
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0xC4); 
    QSPI_DATA_Write(qspi, 0x80); 
    QSPI_Deselect(qspi);

    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x53); 
    QSPI_DATA_Write(qspi, 0x20);
    QSPI_Deselect(qspi);

    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x51);   
    QSPI_DATA_Write(qspi, 0xFF);
    QSPI_Deselect(qspi);

    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, 0x29);  
    QSPI_Deselect(qspi);
    
    sleep_ms(10);
}

/********************************************************************************
function :	Reset the lcd
parameter:
        qspi    ：  qspi structure
********************************************************************************/
static void AMOLED_1IN8_Reset(){
    // Timings from the SH8601 datasheet rather than upstream guesses. This
    // sequence is most of the time it takes an app to appear, and switching
    // apps reboots the chip, so it is on the critical path.
    //
    // Datasheet: reset low pulse width (tRW) minimum is 10 MICROseconds, and
    // secure reset completion (tSRT) is 5ms when reset happens in Sleep In
    // mode, which is the case at power-up. Upstream waits 50ms, 50ms and
    // 300ms: the low pulse alone is five thousand times longer than required
    // and the function costs 400ms. 1ms and 10ms keep a wide margin over the
    // datasheet while giving nearly all of that back.
    gpio_put(qspi.pin_rst,1);
    DEV_Delay_ms(1);
    gpio_put(qspi.pin_rst,0);
    DEV_Delay_ms(1);
    gpio_put(qspi.pin_rst,1);
    DEV_Delay_ms(10);
}

/********************************************************************************
function :	Initialize the lcd
parameter:
        qspi    ：  qspi structure
********************************************************************************/
void AMOLED_1IN8_Init()
{
    //Hardware reset
    AMOLED_1IN8_Reset();
    
    //Set the initialization register
    AMOLED_1IN8_InitReg();

    AMOLED_1IN8.HEIGHT	= AMOLED_1IN8_HEIGHT;
    AMOLED_1IN8.WIDTH   = AMOLED_1IN8_WIDTH;
}

/******************************************************************************
function :	Set AMOLED Brightness
parameter:
******************************************************************************/
void AMOLED_1IN8_SetBrightness(uint8_t brightness){
    if(brightness > 100) brightness = 100;
    brightness = brightness * 255 / 100;

    // QSPI_1Wrie_Mode(&qspi);
    QSPI_Select(qspi); 
    QSPI_REGISTER_Write(qspi, 0x51);
    QSPI_DATA_Write(qspi, brightness);
    QSPI_Deselect(qspi);
}


/******************************************************************************
function :	Clear screen
parameter:
******************************************************************************/
void AMOLED_1IN8_Clear(UWORD Color) {
    // Color data
    UWORD i;
	UWORD image[AMOLED_1IN8.HEIGHT];
	for(i=0;i<AMOLED_1IN8.HEIGHT;i++){
		image[i] = Color>>8 | (Color&0xff)<<8;
	}
	UBYTE *partial_image = (UBYTE *)(image);

    // Send command in one-line mode
    // QSPI_1Wrie_Mode(&qspi);
    AMOLED_1IN8_SetWindows(0,0,AMOLED_1IN8.WIDTH,AMOLED_1IN8.HEIGHT);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi,0x2c);

    // Four-wire mode sends RGB data
    // QSPI_4Wrie_Mode(&qspi);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
    for (int i = 0; i < AMOLED_1IN8.HEIGHT; i++) {
        dma_channel_configure(dma_tx, 
                            &c,
                            &qspi.pio->txf[qspi.sm],  // Destination pointer (PIO TX FIFO)
                            partial_image,            // Source pointer (data buffer)
                            AMOLED_1IN8.WIDTH*2,      // Data length (unit: number of transmissions)
                            true);                    // Start transferring immediately

        // Waiting for DMA transfer to complete
        while(dma_channel_is_busy(dma_tx));
    }

    AMOLED_1IN8_WaitShiftOut();
    QSPI_Deselect(qspi);
}


/******************************************************************************
function :	Send data to AMOLED to complete full screen refresh
parameter:
        Image   ：  Image data
******************************************************************************/
void AMOLED_1IN8_Display(UWORD *Image)
{
    // Send command in one-line mode
    // QSPI_1Wrie_Mode(&qspi);
    AMOLED_1IN8_SetWindows(0,0,AMOLED_1IN8.WIDTH,AMOLED_1IN8.HEIGHT);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi,0x2c);

    // Four-wire mode sends RGB data
    // QSPI_4Wrie_Mode(&qspi);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
    dma_channel_configure(dma_tx, 
                        &c,
                        &qspi.pio->txf[qspi.sm],  // Destination pointer (PIO TX FIFO)
                        (UBYTE *)Image,           // Source pointer (data buffer)
                        AMOLED_1IN8.WIDTH*AMOLED_1IN8.HEIGHT*2,   // Data length (unit: number of transmissions)
                        true);                    // Start transferring immediately
    
    // Waiting for DMA transfer to complete
    while(dma_channel_is_busy(dma_tx));
    AMOLED_1IN8_WaitShiftOut();
    QSPI_Deselect(qspi);
}

/******************************************************************************
function :	Send data to AMOLED to complete partial refresh
parameter:
		Xstart 	:   X direction Start coordinates
		Ystart  :   Y direction Start coordinates
		Xend    :   X direction end coordinates
		Yend    :   Y direction end coordinates
        Image   ：  Image data
******************************************************************************/
void AMOLED_1IN8_DisplayWindows(uint32_t Xstart, uint32_t Ystart, uint32_t Xend, uint32_t Yend, UWORD *Image) {
    
    if(Yend > AMOLED_1IN8.HEIGHT) Yend = AMOLED_1IN8.HEIGHT;
    if(Xend > AMOLED_1IN8.WIDTH) Xend = AMOLED_1IN8.WIDTH;

    // Send command in one-line mode
    // QSPI_1Wrie_Mode(&qspi);
    AMOLED_1IN8_SetWindows(Xstart, Ystart, Xend, Yend);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2c);

    // Four-wire mode sends RGB data
    // QSPI_4Wrie_Mode(&qspi);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));

    int i;
    uint32_t pixel_offset;
    UBYTE *partial_image;
    // Waveshare upstream has `i < Yend - 1` here, which sends one row fewer
    // than SetWindows just declared to the panel (it programs Yend-1 as the
    // inclusive last row), so the bottom row of every partial refresh never
    // updated. Display()/Clear() in this same file use the correct bound.
    for (i = Ystart; i < Yend; i++) {
        pixel_offset = (i * AMOLED_1IN8.WIDTH + Xstart) * 2;
        partial_image = (UBYTE *)Image + pixel_offset;
        dma_channel_configure(dma_tx,
                            &c,
                            &qspi.pio->txf[qspi.sm],  // Destination pointer (PIO TX FIFO)
                            partial_image,            // Source pointer (data buffer)
                            (Xend-Xstart)*2,          // Data length (unit: number of transmissions)
                            true);                    // Start transferring immediately

        // Waiting for DMA transfer to complete
        while(dma_channel_is_busy(dma_tx));
        // dma_channel_is_busy() going false means only that THIS row's bytes
        // have been fed into the PIO TX FIFO, exactly the gap
        // AMOLED_1IN8_WaitShiftOut()'s own header comment describes: the
        // state machine can still be clocking the tail of this row's data
        // (FIFO plus OSR) out onto the wire when the loop immediately
        // reconfigures and re-arms the SAME DMA channel for the next row.
        // That reconfigure was previously unguarded here, row after row, for
        // every partial-window push - the one place in this function the
        // fix below the CS-deselect call was never applied. This is
        // docs/decisions/0001's own named, never-confirmed candidate ("a
        // per-row DMA re-arm race that only bites when the transfer is short
        // enough to complete inside the re-arm window"), closed at the
        // boundary it actually names: between rows, not only at the end of
        // the whole window. Cost is small (busy_wait_us(2) per row, the same
        // margin already paid once per push) against a frame budget in the
        // milliseconds.
        //
        // HONESTY, since this was added chasing a bug it did not fix: the
        // streaks along the palette's rows that prompted this were NOT this
        // race. The owner photographed the panel with this fence in place
        // and they were unchanged; halving the QSPI clock removed them (see
        // firmware/lib/QSPI_PIO/qspi.pio). This guard is kept because the
        // gap it closes is real on its own terms - re-arming a channel
        // whose previous row may still be shifting is wrong whatever else
        // is true - but nothing observed on this board has ever been traced
        // to it, so do not cite it as load-bearing.
        AMOLED_1IN8_WaitShiftOut();
    }

    AMOLED_1IN8_WaitShiftOut();
    QSPI_Deselect(qspi);
}
