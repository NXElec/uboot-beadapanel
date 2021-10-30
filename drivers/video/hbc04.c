/*
 * Display driver for Allwinner SoCs.
 *
 * (C) Copyright 2013-2014 Luc Verhaegen <libv@skynet.be>
 * (C) Copyright 2014-2015 Hans de Goede <hdegoede@redhat.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>

#include <asm/arch/clock.h>

#include <asm/arch/gpio.h>
#include <asm/global_data.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <errno.h>
#include <malloc.h>

struct r61408_config {
	/*********************************************************************/
	/* R61408 configuration                                                                           */
	/*********************************************************************/

	/*
	 * The pins, which are used for SPI communication. This is only used
	 * for configuring R61408, so the performance is irrelevant (only
	 * around a hundred of bytes is moved). Also these can be any arbitrary
	 * GPIO pins (not necessarily the pins having hardware SPI function).
	 * Moreover, the 'sdo' pin may be even not wired up in some devices.
	 *
	 * These configuration variables need to be set as pin numbers for
	 * the standard u-boot GPIO interface (gpio_get_value/gpio_set_value
	 * functions). Note that -1 value can be used for the pins, which are
	 * not really wired up.
	 */
	int csx_pin;
	int sck_pin;
	int sdi_pin;
	int sdo_pin;
	/* SSD2828 reset pin (shared with LCD panel reset) */
	int reset_pin;

	/*
	 * The R61408 has its own dedicated clock source 'tx_clk' (connected
	 * to TX_CLK_XIO/TX_CLK_XIN pins), which is necessary at least for
	 * clocking SPI after reset. The exact clock speed is not strictly,
	 * defined, but the datasheet says that it must be somewhere in the
	 * 8MHz - 30MHz range (see "TX_CLK Timing" section). It can be also
	 * used as a reference clock for PLL. If the exact clock frequency
	 * is known, then it can be specified here. If it is unknown, or the
	 * information is not trustworthy, then it can be set to 0.
	 *
	 * If unsure, set to 0.
	 */
	int ssd2828_tx_clk_khz;

	/*
	 * This is not a property of the used LCD panel, but more like a
	 * property of the SSD2828 wiring. See the "SSD2828QN4 RGB data
	 * arrangement" table in the datasheet. The SSD2828 pins are arranged
	 * in such a way that 18bpp and 24bpp configurations are completely
	 * incompatible with each other.
	 *
	 * Depending on the color depth, this must be set to 16, 18 or 24.
	 */
	int ssd2828_color_depth;

	/*********************************************************************/
	/* LCD panel configuration                                           */
	/*********************************************************************/

	/*
	 * The number of lanes in the MIPI DSI interface. May vary from 1 to 4.
	 *
	 * This information can be found in the LCD panel datasheet.
	 */
	int mipi_dsi_number_of_data_lanes;

	/*
	 * Data transfer bit rate per lane. Please note that it is expected
	 * to be higher than the pixel clock rate of the used video mode when
	 * multiplied by the number of lanes. This is perfectly normal because
	 * MIPI DSI handles data transfers in periodic bursts, and uses the
	 * idle time between bursts for sending configuration information and
	 * commands. Or just for saving power.
	 *
	 * The necessary Mbps/lane information can be found in the LCD panel
	 * datasheet. Note that the transfer rate can't be always set precisely
	 * and it may be rounded *up* (introducing no more than 10Mbps error).
	 */
	int mipi_dsi_bitrate_per_data_lane_mbps;

	/*
	 * Setting this to 1 enforces packing of 18bpp pixel data in 24bpp
	 * envelope when sending it over the MIPI DSI link.
	 *
	 * If unsure, set to 0.
	 */
	int mipi_dsi_loosely_packed_pixel_format;

	/*
	 * According to the "Example for system sleep in and out" section in
	 * the SSD2828 datasheet, some LCD panel specific delays are necessary
	 * after MIPI DCS commands EXIT_SLEEP_MODE and SET_DISPLAY_ON.
	 *
	 * For example, Allwinner uses 100 milliseconds delay after
	 * EXIT_SLEEP_MODE and 200 milliseconds delay after SET_DISPLAY_ON.
	 */
	int mipi_dsi_delay_after_exit_sleep_mode_ms;
	int mipi_dsi_delay_after_set_display_on_ms;
};

//// MUX reg:
#define IOMUXC                           0x020E0000

#define IOMUXC_SW_MUX_CTL_PAD_GPIO1_IO04  0x020E006C
#define IOMUXC_SW_MUX_CTL_PAD_GPIO1_IO03  0x020E0068
#define IOMUXC_SW_MUX_CTL_PAD_GPIO1_IO02  0x020E0064
#define IOMUXC_SW_MUX_CTL_PAD_GPIO1_IO01  0x020E0060
#define IOMUXC_SW_MUX_CTL_PAD_LCD_RESET   0x020E0114

#define IOMUXC_SW_PAD_CTL_PAD_GPIO1_IO04  0x020E02F8
#define IOMUXC_SW_PAD_CTL_PAD_GPIO1_IO03  0x020E02F4
#define IOMUXC_SW_PAD_CTL_PAD_GPIO1_IO02  0x020E02F0
#define IOMUXC_SW_PAD_CTL_PAD_GPIO1_IO01  0x020E02EC
#define IOMUXC_SW_PAD_CTL_PAD_LCD_RESET   0x020E03A0

#define TSXM_CSX0_MUXC 		IOMUXC_SW_MUX_CTL_PAD_GPIO1_IO04
#define TSXP_SDI_MUXC 		IOMUXC_SW_MUX_CTL_PAD_GPIO1_IO03
#define TSYP_SCK_MUXC 		IOMUXC_SW_MUX_CTL_PAD_GPIO1_IO02
#define TSYM_SDO_MUXC 		IOMUXC_SW_MUX_CTL_PAD_GPIO1_IO01
#define MISC1_RESET_MUXC 		IOMUXC_SW_MUX_CTL_PAD_LCD_RESET

//// DATA reg:
#define GPIO1_DR                         0x0209C000
#define GPIO3_DR                         0x020A4000

//// DIR reg:
#define GPIO1_GDIR                       0x0209C004
#define GPIO3_GDIR                       0x020A4004

#define 	GPIO1_IO04_HIGH 		0x00000010
#define 	GPIO1_IO03_HIGH		0x00000008
#define  	GPIO1_IO02_HIGH		0x00000004
#define  	GPIO1_IO01_HIGH		0x00000002
#define 	GPIO3_IO04_HIGH		0x00000010

#define 	GPIO1_IO04_LOW		0xFFFFFFEF
#define 	GPIO1_IO03_LOW		0xFFFFFFF7
#define  	GPIO1_IO02_LOW		0xFFFFFFFB
#define  	GPIO1_IO01_LOW		0xFFFFFFFD
#define 	GPIO3_IO04_LOW		0xFFFFFFEF

#define TSXM_CSX0_GPIO 		GPIO1_DR
#define TSXP_SDI_GPIO 			GPIO1_DR
#define TSYP_SCK_GPIO 			GPIO1_DR
#define TSYM_SDO_GPIO 		GPIO1_DR
#define MISC1_RESET_GPIO 		GPIO3_DR

#define TSXM_CSX0_GPIO_HIGH		GPIO1_IO04_HIGH
#define TSXP_SDI_GPIO_HIGH			GPIO1_IO03_HIGH
#define TSYP_SCK_GPIO_HIGH 		GPIO1_IO02_HIGH
#define TSYM_SDO_GPIO_HIGH		GPIO1_IO01_HIGH
#define MISC1_RESET_GPIO_HIGH		GPIO3_IO04_HIGH

#define TSXM_CSX0_GPIO_LOW		GPIO1_IO04_LOW
#define TSXP_SDI_GPIO_LOW			GPIO1_IO03_LOW
#define TSYP_SCK_GPIO_LOW 			GPIO1_IO02_LOW
#define TSYM_SDO_GPIO_LOW		GPIO1_IO01_LOW
#define MISC1_RESET_GPIO_LOW		GPIO3_IO04_LOW

#define SPI_DELAY_VARIANT 1
#define REG_ADDR(x) 		 (volatile unsigned int *)(x)
#define REG_GET(x) 		*(REG_ADDR(x))

#define LCD_SCL			REG_GET(TSYP_SCK_GPIO)
#define LCD_SCL_HIGH 		TSYP_SCK_GPIO_HIGH
#define LCD_SCL_LOW		TSYP_SCK_GPIO_LOW

#define LCD_SDA			REG_GET(TSXP_SDI_GPIO)
#define LCD_SDA_HIGH 		TSXP_SDI_GPIO_HIGH
#define LCD_SDA_LOW		TSXP_SDI_GPIO_LOW

#define LCD_SDO			REG_GET(TSYM_SDO_GPIO)
#define LCD_SDO_HIGH 		TSYM_SDO_GPIO_HIGH
#define LCD_SDO_LOW		TSYM_SDO_GPIO_LOW

#define LCD_RST			REG_GET(MISC1_RESET_GPIO)
#define LCD_RST_HIGH 		MISC1_RESET_GPIO_HIGH
#define LCD_RST_LOW		MISC1_RESET_GPIO_LOW

#define LCD_CS			REG_GET(TSXM_CSX0_GPIO)
#define LCD_CS_HIGH 		TSXM_CSX0_GPIO_HIGH
#define LCD_CS_LOW		TSXM_CSX0_GPIO_LOW

static struct r61408_config cfg = {
	.csx_pin = IMX_GPIO_NR(1, 4),
	.sck_pin = IMX_GPIO_NR(1, 2),
	.sdi_pin = IMX_GPIO_NR(1, 3),
	.sdo_pin = IMX_GPIO_NR(1, 1),
	.reset_pin = IMX_GPIO_NR(3, 4),	
};
	
static void R61408_Init(struct r61408_config* cfg);
void hbc04_init(void)
{	
//	printf("csx:%x, sck:%x, sdi:%x, sdo:%x, reset:%x\n", cfg.csx_pin, cfg.sck_pin, cfg.sdi_pin, cfg.sdo_pin, cfg.reset_pin);
		
	if (cfg.csx_pin == -1 || cfg.sck_pin == -1 || cfg.sdi_pin == -1) {
		printf("R61408: SPI pins are not properly configured\n");
		return ;
	}
	if (cfg.reset_pin == -1) {
		printf("R61408: Reset pin is not properly configured\n");
		return ;
	}

	R61408_Init(&cfg);
}

static void SPI_WriteComm(unsigned char index)	
{
 unsigned char j;
 
  //************************//

	gpio_set_value(cfg.csx_pin, 0);
	gpio_set_value(cfg.sck_pin, 0);	

	gpio_set_value(cfg.sdi_pin, 0);	
	udelay(SPI_DELAY_VARIANT);
	gpio_set_value(cfg.sck_pin, 1);	      
			    
	udelay(SPI_DELAY_VARIANT);
//	LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
	
	 for(j=0; j<8; j++)
		{
//			LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
		gpio_set_value(cfg.sck_pin, 0);
	
			if(index&0x80)
			{
//				LCD_SDA |= LCD_SDA_HIGH;// Spimosi_High();	
				gpio_set_value(cfg.sdi_pin, 1);	
			}
			else
			{
				gpio_set_value(cfg.sdi_pin, 0);	

			}
			index<<=1;				    
//			LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();      
			
			udelay(SPI_DELAY_VARIANT);
//			LCD_SCL |= LCD_SCL_HIGH;//Spiclk_High();
			gpio_set_value(cfg.sck_pin, 1);		
			udelay(SPI_DELAY_VARIANT);	
		}

//	gpio_set_value(cfg.csx_pin, 1);	
}

static void SPI_WriteData(unsigned char cmddata)	
{
 unsigned char j;
 
  //************************//

	gpio_set_value(cfg.csx_pin, 0);
	gpio_set_value(cfg.sck_pin, 0);	

	gpio_set_value(cfg.sdi_pin, 1);	
	udelay(SPI_DELAY_VARIANT);
	gpio_set_value(cfg.sck_pin, 1);	      
			    
	udelay(SPI_DELAY_VARIANT);
//	LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
	
	 for(j=0; j<8; j++)
	{ 
		gpio_set_value(cfg.sck_pin, 0);
	
			if(cmddata&0x80)
			{	
				gpio_set_value(cfg.sdi_pin, 1);	
			}
			else
			{
				gpio_set_value(cfg.sdi_pin, 0);	
			}
			cmddata<<=1;				       
			
			udelay(SPI_DELAY_VARIANT);

			gpio_set_value(cfg.sck_pin, 1);		
			udelay(SPI_DELAY_VARIANT);	
		}

//	gpio_set_value(cfg.csx_pin, 1);	
}

static unsigned char SPI_ReadData(void)	
{
	unsigned char cmddata = 0;
	unsigned char j;

	gpio_set_value(cfg.csx_pin, 0);
 
  //************************//    
			    
	for(j=0; j<8; j++)
	{
		gpio_set_value(cfg.sck_pin, 0);			    
		udelay(SPI_DELAY_VARIANT);

		cmddata |= gpio_get_value(cfg.sdo_pin)<<(7-j);    			
		gpio_set_value(cfg.sck_pin, 1);		
		udelay(SPI_DELAY_VARIANT);	
	}

//	gpio_set_value(cfg.csx_pin, 1);
	return cmddata;	
}

static int r61408_enable_gpio(const struct r61408_config *cfg)
{
	#if 1
	if (gpio_request(cfg->csx_pin, "r61408_csx")) {
		printf("R61408: request for 'r61408_csx' pin failed\n");
		return 1;
	}
	if (gpio_request(cfg->sck_pin, "r61408_sck")) {
		gpio_free(cfg->csx_pin);
		printf("R61408: request for 'r61408_sck' pin failed\n");
		return 1;
	}
	if (gpio_request(cfg->sdi_pin, "r61408_sdi")) {
		gpio_free(cfg->csx_pin);
		gpio_free(cfg->sck_pin);
		printf("R61408: request for 'r61408_sdi' pin failed\n");
		return 1;
	}
	if (gpio_request(cfg->reset_pin, "r61408_reset")) {
		gpio_free(cfg->csx_pin);
		gpio_free(cfg->sck_pin);
		gpio_free(cfg->sdi_pin);
		printf("R61408: request for 'r61408_reset' pin failed\n");
		return 1;
	}
	if (cfg->sdo_pin != -1 && gpio_request(cfg->sdo_pin, "r61408_sdo")) {
		gpio_free(cfg->csx_pin);
		gpio_free(cfg->sck_pin);
		gpio_free(cfg->sdi_pin);
		gpio_free(cfg->reset_pin);
		printf("R61408: request for 'r61408_sdo' pin failed\n");
		return 1;
	}
#endif
	gpio_direction_output(cfg->reset_pin, 1);
	gpio_direction_output(cfg->csx_pin, 1);
	gpio_direction_output(cfg->sck_pin, 1);
	gpio_direction_output(cfg->sdi_pin, 1);
	if (cfg->sdo_pin != -1)
		gpio_direction_input(cfg->sdo_pin);

	return 0;
}

static void r61529_Reg_Fill(void)
{

	SPI_WriteComm(0xB4);
	SPI_WriteData(0x00);
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0xB0);
	SPI_WriteData(0x04);
	gpio_set_value(cfg.csx_pin, 1);	

	SPI_WriteComm(0x20);
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0x36); //Set_address_mode
 	SPI_WriteData(0x6A); //
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0x3A); 
	SPI_WriteData(0x77);
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0xB3);
	SPI_WriteData(0x02);
	SPI_WriteData(0x00);
	SPI_WriteData(0x00);
	SPI_WriteData(0x20);
	gpio_set_value(cfg.csx_pin, 1);	
	
	
	SPI_WriteComm(0xc0);
	SPI_WriteData(0x03);
	SPI_WriteData(0xdf);
	SPI_WriteData(0x40);
	SPI_WriteData(0x12);
	SPI_WriteData(0x00);
	SPI_WriteData(0x01);
	SPI_WriteData(0x00);
	SPI_WriteData(0x55);// 
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0xc1);
	SPI_WriteData(0x07);
	SPI_WriteData(0x28);
	SPI_WriteData(0x08);// 
	SPI_WriteData(0x08);//
	SPI_WriteData(0x00);//
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0xc4);
	SPI_WriteData(0x70);//
	SPI_WriteData(0x00);
	SPI_WriteData(0x03);
	SPI_WriteData(0x01);
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0xc6);
	SPI_WriteData(0x1d);//1d
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0xc8);
	SPI_WriteData(0x06);
	SPI_WriteData(0x0c);
	SPI_WriteData(0x16);
	SPI_WriteData(0x24);
	SPI_WriteData(0x30);
	SPI_WriteData(0x48);
	SPI_WriteData(0x3d);
	SPI_WriteData(0x28);
	SPI_WriteData(0x20);
	SPI_WriteData(0x14);
	SPI_WriteData(0x0c);
	SPI_WriteData(0x04);
	SPI_WriteData(0x06);
	SPI_WriteData(0x0C);
	SPI_WriteData(0x16);
	SPI_WriteData(0x24);
	SPI_WriteData(0x30);
	SPI_WriteData(0x48);
	SPI_WriteData(0x3D);
	SPI_WriteData(0x28);
	SPI_WriteData(0x20);
	SPI_WriteData(0x14);
	SPI_WriteData(0x0C);
	SPI_WriteData(0x04);
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0xC9);
	SPI_WriteData(0x06);
	SPI_WriteData(0x0C);
	SPI_WriteData(0x16);
	SPI_WriteData(0x24);
	SPI_WriteData(0x30);
	SPI_WriteData(0x48);
	SPI_WriteData(0x3D);
	SPI_WriteData(0x28);
	SPI_WriteData(0x20);
	SPI_WriteData(0x14);
	SPI_WriteData(0x0C);
	SPI_WriteData(0x04);	
	SPI_WriteData(0x06);
	SPI_WriteData(0x0C);
	SPI_WriteData(0x16);
	SPI_WriteData(0x24);
	SPI_WriteData(0x30);
	SPI_WriteData(0x48);
	SPI_WriteData(0x3D);
	SPI_WriteData(0x28);
	SPI_WriteData(0x20);
	SPI_WriteData(0x14);
	SPI_WriteData(0x0C);
	SPI_WriteData(0x04);
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0xCA);
	SPI_WriteData(0x06);
	SPI_WriteData(0x0C);
	SPI_WriteData(0x16);
	SPI_WriteData(0x24);
	SPI_WriteData(0x30);
	SPI_WriteData(0x48);
	SPI_WriteData(0x3D);
	SPI_WriteData(0x28);
	SPI_WriteData(0x20);
	SPI_WriteData(0x14);
	SPI_WriteData(0x0C);
	SPI_WriteData(0x04);	
	SPI_WriteData(0x06);
	SPI_WriteData(0x0C);
	SPI_WriteData(0x16);
	SPI_WriteData(0x24);
	SPI_WriteData(0x30);
	SPI_WriteData(0x48);
	SPI_WriteData(0x3D);
	SPI_WriteData(0x28);
	SPI_WriteData(0x20);
	SPI_WriteData(0x14);
	SPI_WriteData(0x0C);
	SPI_WriteData(0x04);
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0xD0);
	SPI_WriteData(0x95);
	SPI_WriteData(0x0A);
	SPI_WriteData(0x08);
	SPI_WriteData(0x10);
	SPI_WriteData(0x39);
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0xD1);
	SPI_WriteData(0x02);
	SPI_WriteData(0x2c);//
	SPI_WriteData(0x2c);//
	SPI_WriteData(0x44);//	
	SPI_WriteData(0x00);//0x08 
	gpio_set_value(cfg.csx_pin, 1);
		
	SPI_WriteComm(0x11);
	gpio_set_value(cfg.csx_pin, 1);

	mdelay(7);
	SPI_WriteComm(0x29);
	gpio_set_value(cfg.csx_pin, 1);

	mdelay(7);
	SPI_WriteComm(0x2C);
	gpio_set_value(cfg.csx_pin, 1);
			
	SPI_WriteComm(0x36);
	SPI_WriteData(0x00);
	gpio_set_value(cfg.csx_pin, 1);
			
}

static void r61408_Reg_Fill(void)
{
	////////////////////SSD2828 LP/////////////////////////
		
SPI_WriteComm(0x11); 
mdelay(40);
SPI_WriteComm(0xB0);
SPI_WriteData(0x04);
	
SPI_WriteComm(0xB3);
SPI_WriteData(0x10);//0x02
SPI_WriteData(0x00);
SPI_WriteData(0x00);

SPI_WriteComm(0xB6);
SPI_WriteData(0x52);
SPI_WriteData(0x83);

SPI_WriteComm(0xB7);
SPI_WriteData(0x80);
SPI_WriteData(0x72);
SPI_WriteData(0x11);
SPI_WriteData(0x25);

SPI_WriteComm(0xB8);
SPI_WriteData(0x00);
SPI_WriteData(0x0F);
SPI_WriteData(0x0F);
SPI_WriteData(0xFF);
SPI_WriteData(0xFF);
SPI_WriteData(0xC8);
SPI_WriteData(0xC8);
SPI_WriteData(0x02);
SPI_WriteData(0x18);
SPI_WriteData(0x10);
SPI_WriteData(0x10);
SPI_WriteData(0x37);
SPI_WriteData(0x5A);
SPI_WriteData(0x87);
SPI_WriteData(0xBE);
SPI_WriteData(0xFF);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteComm(0xB9);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteComm(0xBD);
SPI_WriteData(0x00);

SPI_WriteComm(0xC0);
SPI_WriteData(0x02);
SPI_WriteData(0x76);

SPI_WriteComm(0xC1);
SPI_WriteData(0x63);
SPI_WriteData(0x31);
SPI_WriteData(0x00);
SPI_WriteData(0x27);
SPI_WriteData(0x27);
SPI_WriteData(0x32);
SPI_WriteData(0x12);
SPI_WriteData(0x28);
SPI_WriteData(0x4E);
SPI_WriteData(0x10);
SPI_WriteData(0xA5);
SPI_WriteData(0x0F);
SPI_WriteData(0x58);
SPI_WriteData(0x21);
SPI_WriteData(0x01);

SPI_WriteComm(0xC2);
SPI_WriteData(0x28);
SPI_WriteData(0x06);
SPI_WriteData(0x06);
SPI_WriteData(0x01);
SPI_WriteData(0x03);
SPI_WriteData(0x00);

SPI_WriteComm(0xC3);
SPI_WriteData(0x40);
SPI_WriteData(0x00);
SPI_WriteData(0x03);
SPI_WriteComm(0xC4);
SPI_WriteData(0x00);
SPI_WriteData(0x01);
SPI_WriteComm(0xC6);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteComm(0xC7);
SPI_WriteData(0x11);
SPI_WriteData(0x8D);
SPI_WriteData(0xA0);
SPI_WriteData(0xF5);
SPI_WriteData(0x27);
SPI_WriteComm(0xC8);
SPI_WriteData(0x02);
SPI_WriteData(0x13);
SPI_WriteData(0x18);
SPI_WriteData(0x25);
SPI_WriteData(0x34);
SPI_WriteData(0x4E);
SPI_WriteData(0x36);
SPI_WriteData(0x23);
SPI_WriteData(0x17);
SPI_WriteData(0x0E);
SPI_WriteData(0x0C);
SPI_WriteData(0x02);
SPI_WriteData(0x02);
SPI_WriteData(0x13);
SPI_WriteData(0x18);
SPI_WriteData(0x25);
SPI_WriteData(0x34);
SPI_WriteData(0x4E);
SPI_WriteData(0x36);
SPI_WriteData(0x23);
SPI_WriteData(0x17);
SPI_WriteData(0x0E);
SPI_WriteData(0x0C);
SPI_WriteData(0x02);
SPI_WriteComm(0xC9);
SPI_WriteData(0x02);
SPI_WriteData(0x13);
SPI_WriteData(0x18);
SPI_WriteData(0x25);
SPI_WriteData(0x34);
SPI_WriteData(0x4E);
SPI_WriteData(0x36);
SPI_WriteData(0x23);
SPI_WriteData(0x17);
SPI_WriteData(0x0E);
SPI_WriteData(0x0C);
SPI_WriteData(0x02);
SPI_WriteData(0x02);
SPI_WriteData(0x13);
SPI_WriteData(0x18);
SPI_WriteData(0x25);
SPI_WriteData(0x34);
SPI_WriteData(0x4E);
SPI_WriteData(0x36);
SPI_WriteData(0x23);
SPI_WriteData(0x17);
SPI_WriteData(0x0E);
SPI_WriteData(0x0C);
SPI_WriteData(0x02);
SPI_WriteComm(0xCA);
SPI_WriteData(0x02);
SPI_WriteData(0x13);
SPI_WriteData(0x18);
SPI_WriteData(0x25);
SPI_WriteData(0x34);
SPI_WriteData(0x4E);
SPI_WriteData(0x36);
SPI_WriteData(0x23);
SPI_WriteData(0x17);
SPI_WriteData(0x0E);
SPI_WriteData(0x0C);
SPI_WriteData(0x02);
SPI_WriteData(0x02);
SPI_WriteData(0x13);
SPI_WriteData(0x18);
SPI_WriteData(0x25);
SPI_WriteData(0x34);
SPI_WriteData(0x4E);
SPI_WriteData(0x36);
SPI_WriteData(0x23);
SPI_WriteData(0x17);
SPI_WriteData(0x0E);
SPI_WriteData(0x0C);
SPI_WriteData(0x02);
SPI_WriteComm(0xD0);
SPI_WriteData(0xA9);
SPI_WriteData(0x03);
SPI_WriteData(0xCC);
SPI_WriteData(0xA5);
SPI_WriteData(0x00);
SPI_WriteData(0x53);
SPI_WriteData(0x20);
SPI_WriteData(0x10);
SPI_WriteData(0x01);
SPI_WriteData(0x00);
SPI_WriteData(0x01);
SPI_WriteData(0x01);
SPI_WriteData(0x00);
SPI_WriteData(0x03);
SPI_WriteData(0x01);
SPI_WriteData(0x00);
SPI_WriteComm(0xD1);
SPI_WriteData(0x18);
SPI_WriteData(0x0C);
SPI_WriteData(0x23);
SPI_WriteData(0x03);
SPI_WriteData(0x75);
SPI_WriteData(0x02);
SPI_WriteData(0x50);
SPI_WriteComm(0xD3);
SPI_WriteData(0x33);
SPI_WriteComm(0xD5);
SPI_WriteData(0x2a);
SPI_WriteData(0x2a);
SPI_WriteComm(0xD6);
SPI_WriteData(0x28);//a8
SPI_WriteComm(0xD7);
SPI_WriteData(0x01);
SPI_WriteData(0x00);
SPI_WriteData(0xAA);
SPI_WriteData(0xC0);
SPI_WriteData(0x2A);
SPI_WriteData(0x2C);
SPI_WriteData(0x22);
SPI_WriteData(0x12);
SPI_WriteData(0x71);
SPI_WriteData(0x0A);
SPI_WriteData(0x12);
SPI_WriteData(0x00);
SPI_WriteData(0xA0);
SPI_WriteData(0x00);
SPI_WriteData(0x03);
SPI_WriteComm(0xD8);
SPI_WriteData(0x44);
SPI_WriteData(0x44);
SPI_WriteData(0x22);
SPI_WriteData(0x44);
SPI_WriteData(0x21);
SPI_WriteData(0x46);
SPI_WriteData(0x42);
SPI_WriteData(0x40);
SPI_WriteComm(0xD9);
SPI_WriteData(0xCF);
SPI_WriteData(0x2D);
SPI_WriteData(0x51);
SPI_WriteComm(0xDA);
SPI_WriteData(0x01);
SPI_WriteComm(0xDE);
SPI_WriteData(0x01);
SPI_WriteData(0x51);//58
SPI_WriteComm(0xE1);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteComm(0xE6);
SPI_WriteData(0x55);//58
SPI_WriteComm(0xF3);
SPI_WriteData(0x06);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x24);
SPI_WriteData(0x00);
SPI_WriteComm(0xF8);
SPI_WriteData(0x00);
SPI_WriteComm(0xFA);
SPI_WriteData(0x01);
SPI_WriteComm(0xFB);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteComm(0xFC);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteComm(0xFD);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x70);
SPI_WriteData(0x00);
SPI_WriteData(0x72);
SPI_WriteData(0x31);
SPI_WriteData(0x37);
SPI_WriteData(0x70);
SPI_WriteData(0x32);
SPI_WriteData(0x31);
SPI_WriteData(0x07);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteComm(0xFE);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x20);
SPI_WriteComm(0xB0);
SPI_WriteData(0x04); //04
mdelay(40);
SPI_WriteComm(0x35);
SPI_WriteData(0x00);
SPI_WriteComm(0x44);
SPI_WriteData(0x00);
SPI_WriteComm(0x36);
SPI_WriteData(0x00);
SPI_WriteComm(0x3A);
SPI_WriteData(0x77);
SPI_WriteComm(0x2A);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x01);
SPI_WriteData(0xDF);
SPI_WriteComm(0x2B);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x03);
SPI_WriteData(0x1F);
SPI_WriteComm(0x29);    
mdelay(10);
SPI_WriteComm(0x2C);
mdelay(10); 
//	Lcd_Light_ON;

SPI_WriteComm(0x36);
SPI_WriteData(0x08);

}

static void R61408_Init(struct r61408_config* cfg)
{	
	unsigned char byte1,byte2;
	
	/* Initialize the pins */
	if (r61408_enable_gpio(cfg) != 0)
		return ;

	gpio_set_value(cfg->csx_pin, 1);
	mdelay(2);
	gpio_set_value(cfg->csx_pin, 0);
	gpio_set_value(cfg->reset_pin, 0);
	mdelay(10);
	gpio_set_value(cfg->reset_pin, 1);
	mdelay(10);

	gpio_set_value(cfg->csx_pin, 1);

	// Enter read mode
	SPI_WriteComm(0xB0);
	SPI_WriteData(0x04);
	gpio_set_value(cfg->csx_pin, 1);
	
	SPI_WriteComm(0xBF);
	SPI_ReadData();

	SPI_ReadData();
	SPI_ReadData();
	byte1 = SPI_ReadData();
	byte2 = SPI_ReadData();
	gpio_set_value(cfg->csx_pin, 1);
	
	// Enter read mode
	SPI_WriteComm(0xB0);
	SPI_WriteData(0x04);
	gpio_set_value(cfg->csx_pin, 1);
		
  if (byte1==0x15 && byte2==0x29) {
  	printf("R61259 found!\n");
		r61529_Reg_Fill();

  }

	gpio_set_value(cfg->csx_pin, 1);  			  	

}
