/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/* Configured for dap-firmware — see art.c.     */
/*----------------------------------------------*/

#define	JD_SZBUF		512
/* Size of stream input buffer. 512 matches the SD sector size, so one refill
/  is one FatFs read. */

#define JD_FORMAT		0
/* 0: RGB888. The decoder resamples covers down to screen size and wants
/  8 bits per channel to average with; RGB565 happens once, at the end. */

#define	JD_USE_SCALE	1
/* 1/2, 1/4, 1/8 descaling during decode — a 1400 px cover never has to be
/  decoded at full size to end up at 216 px. */

#define JD_TBLCLIP		1
/* Table saturation: a bit faster for 1 KB of flash. */

#define JD_FASTDECODE	2
/* 32-bit barrel shifter plus table Huffman decoding. Needs ~9.6 KB of
/  workspace, which the H7 has to spare, and makes decode noticeably
/  faster — and decode time is time the audio ring is not being topped up
/  in big chunks. */
