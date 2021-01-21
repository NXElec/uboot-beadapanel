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

struct ssd2828_config {
	/*********************************************************************/
	/* SSD2828 configuration                                             */
	/*********************************************************************/

	/*
	 * The pins, which are used for SPI communication. This is only used
	 * for configuring SSD2828, so the performance is irrelevant (only
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
	 * The SSD2828 has its own dedicated clock source 'tx_clk' (connected
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

#define SPI_DELAY_VARIANT 10
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

static struct ssd2828_config *cfgp;

	struct ssd2828_config cfg = {

		.ssd2828_color_depth = 24,

		.mipi_dsi_number_of_data_lanes           = 4,
		.mipi_dsi_bitrate_per_data_lane_mbps     = 513,
		.mipi_dsi_delay_after_exit_sleep_mode_ms = 100,
		.mipi_dsi_delay_after_set_display_on_ms  = 200
	};

	
void HX8394_Init(struct ssd2828_config* cfg);
void hbc068_init(void)
{
		cfg.csx_pin = IMX_GPIO_NR(1, 4),
		cfg.sck_pin = IMX_GPIO_NR(1, 2),
		cfg.sdi_pin = IMX_GPIO_NR(1, 3),
		cfg.sdo_pin = IMX_GPIO_NR(1, 1),
		cfg.reset_pin = IMX_GPIO_NR(3, 4),
	cfgp = &cfg;
	
	printf("csx:%x, sck:%x, sdi:%x, reset:%x\n", cfg.csx_pin, cfg.sck_pin, cfg.sdi_pin, cfg.reset_pin);
		
	if (cfg.csx_pin == -1 || cfg.sck_pin == -1 || cfg.sdi_pin == -1) {
		printf("SSD2828: SPI pins are not properly configured\n");
		return ;
	}
	if (cfg.reset_pin == -1) {
		printf("SSD2828: Reset pin is not properly configured\n");
		return ;
	}

	HX8394_Init(cfgp);

}


#if 1
void SPI_WriteCmd(unsigned char index)	
{
 unsigned char j;
 
  //************************//

	gpio_set_value(cfgp->csx_pin, 0);
	gpio_set_value(cfgp->sck_pin, 0);	

	gpio_set_value(cfgp->sdi_pin, 0);	
	udelay(SPI_DELAY_VARIANT);
	gpio_set_value(cfgp->sck_pin, 1);	      
			    
	udelay(SPI_DELAY_VARIANT);
//	LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
	
	 for(j=0; j<8; j++)
		{
//			LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
		gpio_set_value(cfgp->sck_pin, 0);
	
			if(index&0x80)
			{
//				LCD_SDA |= LCD_SDA_HIGH;// Spimosi_High();	
				gpio_set_value(cfgp->sdi_pin, 1);	
			}
			else
			{
				gpio_set_value(cfgp->sdi_pin, 0);	

			}
			index<<=1;				    
//			LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();      
			
			udelay(SPI_DELAY_VARIANT);
//			LCD_SCL |= LCD_SCL_HIGH;//Spiclk_High();
			gpio_set_value(cfgp->sck_pin, 1);		
			udelay(SPI_DELAY_VARIANT);	
		}

	gpio_set_value(cfgp->sdi_pin, 1);	
}

void SPI_WriteData(unsigned char cmddata)	
{
 unsigned char j;
 
  //************************//

	gpio_set_value(cfgp->csx_pin, 0);
	gpio_set_value(cfgp->sck_pin, 0);	

	gpio_set_value(cfgp->sdi_pin, 1);	
	udelay(SPI_DELAY_VARIANT);
	gpio_set_value(cfgp->sck_pin, 1);	      
			    
	udelay(SPI_DELAY_VARIANT);
//	LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
	
	 for(j=0; j<8; j++)
		{
//			LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
		gpio_set_value(cfgp->sck_pin, 0);
	
			if(cmddata&0x80)
			{
//				LCD_SDA |= LCD_SDA_HIGH;// Spimosi_High();	
				gpio_set_value(cfgp->sdi_pin, 1);	
			}
			else
			{
				gpio_set_value(cfgp->sdi_pin, 0);	

			}
			cmddata<<=1;				    
//			LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();      
			
			udelay(SPI_DELAY_VARIANT);
//			LCD_SCL |= LCD_SCL_HIGH;//Spiclk_High();
			gpio_set_value(cfgp->sck_pin, 1);		
			udelay(SPI_DELAY_VARIANT);	
		}

	gpio_set_value(cfgp->sdi_pin, 1);	
}
#else

void SPI_WriteCmd(unsigned char index)	
{
 register unsigned char j;
 register unsigned int k;

  //************************//

	k = LCD_CS;
	k &= LCD_CS_LOW;//Spiclk_High(); 
	LCD_CS = k;  	
	k = LCD_SCL;
	k &= LCD_SCL_LOW;
	LCD_SCL = k;		

	k = LCD_SDA;
	k &= LCD_SDA_LOW;//Spiclk_High(); 
	LCD_SDA = k;  	
//	udelay(SPI_DELAY_VARIANT);
	k = LCD_SCL;
	k |= LCD_SCL_HIGH;
	LCD_SCL = k;	      
			    
//	udelay(SPI_DELAY_VARIANT);
//	LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
	
	 for(j=0; j<8; j++)
		{
//			LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
	k = LCD_SCL;
	k &= LCD_SCL_LOW;
	LCD_SCL = k;	
	
			if(index&0x80)
			{
//				LCD_SDA |= LCD_SDA_HIGH;// Spimosi_High();	
	k = LCD_SDA;
	k |= LCD_SDA_HIGH;
	LCD_SDA = k;	      
			}
			else
			{
	k = LCD_SDA;
	k &= LCD_SDA_LOW;
	LCD_SDA = k;	 
			}
			index<<=1;				    
//			LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();      
			
//			udelay(SPI_DELAY_VARIANT);
//			LCD_SCL |= LCD_SCL_HIGH;//Spiclk_High();
	k = LCD_SCL;
	k |= LCD_SCL_HIGH;
	LCD_SCL = k;			
//			udelay(SPI_DELAY_VARIANT);	
		}

	k = LCD_SDA;
	k |= LCD_SDA_HIGH;
	LCD_SDA = k;	 
	
//	udelay(SPI_DELAY_VARIANT);			 
}

void SPI_WriteData(unsigned char cmddata)	
{
  register unsigned char j;
 register unsigned int k;

  //************************//

	k = LCD_CS;
	k &= LCD_CS_LOW;//Spiclk_High(); 
	LCD_CS = k;  	
	k = LCD_SCL;
	k &= LCD_SCL_LOW;
	LCD_SCL = k;		

	k = LCD_SDA;
	k |= LCD_SDA_HIGH;//Spiclk_High(); 
	LCD_SDA = k;  	
//	udelay(SPI_DELAY_VARIANT);
	k = LCD_SCL;
	k |= LCD_SCL_HIGH;
	LCD_SCL = k;	      
			    
//	udelay(SPI_DELAY_VARIANT);
//	LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
	
	 for(j=0; j<8; j++)
		{
//			LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();       
	k = LCD_SCL;
	k &= LCD_SCL_LOW;
	LCD_SCL = k;	
	
			if(cmddata&0x80)
			{
//				LCD_SDA |= LCD_SDA_HIGH;// Spimosi_High();	
	k = LCD_SDA;
	k |= LCD_SDA_HIGH;
	LCD_SDA = k;	      
			}
			else
			{
	k = LCD_SDA;
	k &= LCD_SDA_LOW;
	LCD_SDA = k;	 
			}
			cmddata<<=1;				    
//			LCD_SCL &= LCD_SCL_LOW;//Spiclk_High();      
			
//			udelay(SPI_DELAY_VARIANT);
//			LCD_SCL |= LCD_SCL_HIGH;//Spiclk_High();
	k = LCD_SCL;
	k |= LCD_SCL_HIGH;
	LCD_SCL = k;			
//			udelay(SPI_DELAY_VARIANT);	
		}

	k = LCD_SDA;
	k |= LCD_SDA_HIGH;
	LCD_SDA = k;	
	
//	udelay(SPI_DELAY_VARIANT);	 
}
#endif

void SSD2828_WritePackageSize(unsigned char PCSdata)
{
//unsigned int i;
//unsigned char  PCS;
//PCS=*Data1;
        SPI_WriteCmd(0xB7);
        SPI_WriteData(0x50);
        SPI_WriteData(0x02);

        SPI_WriteCmd(0xBD);
        SPI_WriteData(0x00);
        SPI_WriteData(0x00);

        SPI_WriteCmd(0xBC);
        SPI_WriteData(PCSdata);
        SPI_WriteData(0x00);
		//delay1(1);
        SPI_WriteCmd(0xbf);
        //for(i=0;i<PCS;i++){SPI_WriteData(*(Data1+i+1));}
}

//Timing parameter for6.86 LCD
#define VBPD_MIPI 		(15)			//垂直同步信号的后肩14
#define VFPD_MIPI 		(2)			//垂直同步信号的前肩16
#define VSPW_MIPI  		(10)			//垂直同步信号的脉宽2

#define HBPD_MIPI  		(120)			//水平同步信号的后肩42
#define HFPD_MIPI  		(120)			//水平同步信号的前肩44
#define HSPW_MIPI  		(3)			//水平同步信号的脉宽6  

static int ssd2828_enable_gpio(const struct ssd2828_config *cfg)
{
	#if 1
	if (gpio_request(cfg->csx_pin, "ssd2828_csx")) {
		printf("SSD2828: request for 'ssd2828_csx' pin failed\n");
		return 1;
	}
	if (gpio_request(cfg->sck_pin, "ssd2828_sck")) {
		gpio_free(cfg->csx_pin);
		printf("SSD2828: request for 'ssd2828_sck' pin failed\n");
		return 1;
	}
	if (gpio_request(cfg->sdi_pin, "ssd2828_sdi")) {
		gpio_free(cfg->csx_pin);
		gpio_free(cfg->sck_pin);
		printf("SSD2828: request for 'ssd2828_sdi' pin failed\n");
		return 1;
	}
	if (gpio_request(cfg->reset_pin, "ssd2828_reset")) {
		gpio_free(cfg->csx_pin);
		gpio_free(cfg->sck_pin);
		gpio_free(cfg->sdi_pin);
		printf("SSD2828: request for 'ssd2828_reset' pin failed\n");
		return 1;
	}
	if (cfg->sdo_pin != -1 && gpio_request(cfg->sdo_pin, "ssd2828_sdo")) {
		gpio_free(cfg->csx_pin);
		gpio_free(cfg->sck_pin);
		gpio_free(cfg->sdi_pin);
		gpio_free(cfg->reset_pin);
		printf("SSD2828: request for 'ssd2828_sdo' pin failed\n");
		return 1;
	}
#endif
	gpio_direction_output(cfg->reset_pin, 0);
	gpio_direction_output(cfg->csx_pin, 1);
	gpio_direction_output(cfg->sck_pin, 1);
	gpio_direction_output(cfg->sdi_pin, 1);
	if (cfg->sdo_pin != -1)
		gpio_direction_input(cfg->sdo_pin);

	return 0;
}

void HX8394_Init(struct ssd2828_config* cfg)

{	
	/* Initialize the pins */
	if (ssd2828_enable_gpio(cfg) != 0)
		return ;

	gpio_set_value(cfg->csx_pin, 1);
	gpio_set_value(cfg->reset_pin, 1);
	mdelay(5);
	gpio_set_value(cfg->reset_pin, 0);
	mdelay(50);
	gpio_set_value(cfg->reset_pin, 1);
	mdelay(150);

	gpio_set_value(cfg->sck_pin, 0);	
	
	////////////////////SSD2828 LP/////////////////////////
	
	
	 SPI_WriteCmd(0xb7);
   SPI_WriteData(0x50);//50=TX_CLK 70=PCLK
   SPI_WriteData(0x00);   //Configuration Register

   SPI_WriteCmd(0xb8);
   SPI_WriteData(0x00);
   SPI_WriteData(0x00);   //VC(Virtual ChannelID) Control Register

   SPI_WriteCmd(0xb9);
   SPI_WriteData(0x00);//1=PLL disable
   SPI_WriteData(0x00);
                               //TX_CLK/MS should be between 5Mhz to100Mhz
   SPI_WriteCmd(0xBA);//PLL=(TX_CLK/MS)*NS 8228=480M 4428=240M  061E=120M 4214=240M 821E=360M 8219=300M
   SPI_WriteData(0x14);//D7-0=NS(0x01 : NS=1)
   SPI_WriteData(0x42);//D15-14=PLL范围 00=62.5-125 01=126-250 10=251-500 11=501-1000  DB12-8=MS(01:MS=1)

   SPI_WriteCmd(0xBB);//LP Clock Divider LP clock = 400MHz / LPD / 8 = 240 / 8 / 4 = 7.5MHz
   SPI_WriteData(0x04);//D5-0=LPD=0x1 – Divide by 2
   SPI_WriteData(0x00);

 //  SPI_WriteCmd(0xb9);
 //  SPI_WriteData(0x01);//1=PLL disable
 //  SPI_WriteData(0x00);
       //MIPI lane configuration
   SPI_WriteCmd(0xDE);//通道数
   SPI_WriteData(0x03);//11=4LANE 10=3LANE 01=2LANE 00=1LANE
   SPI_WriteData(0x00);

   SPI_WriteCmd(0xc9);
   SPI_WriteData(0x02);
   SPI_WriteData(0x23);   //p1: HS-Data-zero  p2: HS-Data- prepare  --> 8031 issue
                //Delay(100);
	
	
	
//////////////////////////////////////////////

	
SSD2828_WritePackageSize(4);//HX8394-D  6.86BOE
SPI_WriteData(0xB9);
SPI_WriteData(0xFF);
SPI_WriteData(0x83);
SPI_WriteData(0x94);

SSD2828_WritePackageSize(3);
SPI_WriteData(0xBA);
SPI_WriteData(0x73);
SPI_WriteData(0x83);

// Set Power HX5186 Mode /External Power Mode
SSD2828_WritePackageSize(16);
SPI_WriteData(0xB1);
SPI_WriteData(0x6C);
SPI_WriteData(0x0C);
SPI_WriteData(0x0D);
SPI_WriteData(0x25);
SPI_WriteData(0x04);
SPI_WriteData(0x11);
SPI_WriteData(0xF1);
SPI_WriteData(0x81);
SPI_WriteData(0x5C);
SPI_WriteData(0xE6);
SPI_WriteData(0x23);
SPI_WriteData(0x80);
SPI_WriteData(0xC0);
SPI_WriteData(0xD2);
SPI_WriteData(0x58);

SSD2828_WritePackageSize(13);
SPI_WriteData(0xB2);
SPI_WriteData(0x00);
SPI_WriteData(0x64);
SPI_WriteData(0x0F);
SPI_WriteData(0x09);
SPI_WriteData(0x24);
SPI_WriteData(0x1C);
SPI_WriteData(0x08);
SPI_WriteData(0x08);
SPI_WriteData(0x1C);
SPI_WriteData(0x4D);
SPI_WriteData(0x00);
SPI_WriteData(0x00);

SSD2828_WritePackageSize(13);
SPI_WriteData(0xB4);
SPI_WriteData(0x00);
SPI_WriteData(0xFF);
SPI_WriteData(0x01);
SPI_WriteData(0x5A);
SPI_WriteData(0x01);
SPI_WriteData(0x5A);
SPI_WriteData(0x01);
SPI_WriteData(0x5A);
SPI_WriteData(0x01);
SPI_WriteData(0x6C);
SPI_WriteData(0x01);
SPI_WriteData(0x6C);


// Set Power Option HX5186 Mode
SSD2828_WritePackageSize(4);
SPI_WriteData(0xBF);
SPI_WriteData(0x41);
SPI_WriteData(0x0E);
SPI_WriteData(0x01);

SSD2828_WritePackageSize(33);
SPI_WriteData(0xD3);
SPI_WriteData(0x00);
SPI_WriteData(0x07);
SPI_WriteData(0x00);
SPI_WriteData(0x64);
SPI_WriteData(0x07);
SPI_WriteData(0x08);
SPI_WriteData(0x08);
SPI_WriteData(0x32);
SPI_WriteData(0x10);
SPI_WriteData(0x07);
SPI_WriteData(0x00);
SPI_WriteData(0x07);
SPI_WriteData(0x32);
SPI_WriteData(0x10);
SPI_WriteData(0x03);
SPI_WriteData(0x00);
SPI_WriteData(0x03);
SPI_WriteData(0x00);
SPI_WriteData(0x32);
SPI_WriteData(0x10);
SPI_WriteData(0x08);
SPI_WriteData(0x00);
SPI_WriteData(0x35);
SPI_WriteData(0x33);
SPI_WriteData(0x09);
SPI_WriteData(0x09);
SPI_WriteData(0x37);
SPI_WriteData(0x0D);
SPI_WriteData(0x07);
SPI_WriteData(0x37);
SPI_WriteData(0x0E);
SPI_WriteData(0x08);

// Set GIP

SSD2828_WritePackageSize(45);
SPI_WriteData(0xD5);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x24);
SPI_WriteData(0x24);
SPI_WriteData(0x1A);
SPI_WriteData(0x1A);
SPI_WriteData(0x1B);
SPI_WriteData(0x1B);
SPI_WriteData(0x04);
SPI_WriteData(0x05);
SPI_WriteData(0x06);
SPI_WriteData(0x07);
SPI_WriteData(0x00);
SPI_WriteData(0x01);
SPI_WriteData(0x02);
SPI_WriteData(0x03);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x19);
SPI_WriteData(0x19);
SPI_WriteData(0x20);
SPI_WriteData(0x21);
SPI_WriteData(0x22);
SPI_WriteData(0x23);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);

SSD2828_WritePackageSize(45);
SPI_WriteData(0xD6);
SPI_WriteData(0x19);
SPI_WriteData(0x19);
SPI_WriteData(0x24);
SPI_WriteData(0x24);
SPI_WriteData(0x1A);
SPI_WriteData(0x1A);
SPI_WriteData(0x1B);
SPI_WriteData(0x1B);
SPI_WriteData(0x03);
SPI_WriteData(0x02);
SPI_WriteData(0x01);
SPI_WriteData(0x00);
SPI_WriteData(0x07);
SPI_WriteData(0x06);
SPI_WriteData(0x05);
SPI_WriteData(0x04);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x23);
SPI_WriteData(0x22);
SPI_WriteData(0x21);
SPI_WriteData(0x20);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);
SPI_WriteData(0x18);

// Set Gamma
SSD2828_WritePackageSize(43);
SPI_WriteData(0xE0);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x02);
SPI_WriteData(0x28);
SPI_WriteData(0x2D);
SPI_WriteData(0x3D);
SPI_WriteData(0x0F);
SPI_WriteData(0x32);
SPI_WriteData(0x06);
SPI_WriteData(0x09);
SPI_WriteData(0x0C);
SPI_WriteData(0x17);
SPI_WriteData(0x0E);
SPI_WriteData(0x12);
SPI_WriteData(0x14);
SPI_WriteData(0x12);
SPI_WriteData(0x14);
SPI_WriteData(0x07);
SPI_WriteData(0x11);
SPI_WriteData(0x12);
SPI_WriteData(0x18);
SPI_WriteData(0x00);
SPI_WriteData(0x00);
SPI_WriteData(0x03);
SPI_WriteData(0x28);
SPI_WriteData(0x2C);
SPI_WriteData(0x3D);
SPI_WriteData(0x0F);
SPI_WriteData(0x32);
SPI_WriteData(0x06);
SPI_WriteData(0x09);
SPI_WriteData(0x0B);
SPI_WriteData(0x16);
SPI_WriteData(0x0F);
SPI_WriteData(0x11);
SPI_WriteData(0x14);
SPI_WriteData(0x13);
SPI_WriteData(0x13);
SPI_WriteData(0x07);
SPI_WriteData(0x11);
SPI_WriteData(0x11);
SPI_WriteData(0x17);

// Set Panel
SSD2828_WritePackageSize(2);
SPI_WriteData(0xCC);
SPI_WriteData(0x01);//05 fan sao

//SSD2828_WritePackageSize(2);
//SPI_WriteData(0x36);
//SPI_WriteData(0x14);


// Set TCON Option
SSD2828_WritePackageSize(5);
SPI_WriteData(0xC7);
SPI_WriteData(0x00);
SPI_WriteData(0xC0);
SPI_WriteData(0x40);
SPI_WriteData(0xC0);

// Set C0
SSD2828_WritePackageSize(3);
SPI_WriteData(0xC0);
SPI_WriteData(0x30);
SPI_WriteData(0x14);

// Set VCOM	 2020-03-13
SSD2828_WritePackageSize(3);
SPI_WriteData(0xB6);
SPI_WriteData(0x46);//46  55
SPI_WriteData(0x46);//46  55

// Sleep Out


// Set ECO
SSD2828_WritePackageSize(3);
SPI_WriteData(0xC6);
SPI_WriteData(0x3D);
SPI_WriteData(0x00);


SSD2828_WritePackageSize(2);
SPI_WriteData(0x3A);//RGB565 MODE
SPI_WriteData(0x50);

SSD2828_WritePackageSize(2);
SPI_WriteData(0x36);//RGB565 MODE
SPI_WriteData(0x01);

SSD2828_WritePackageSize(1);
SPI_WriteData(0x11);

SSD2828_WritePackageSize(1);
SPI_WriteData(0x29);

SSD2828_WritePackageSize(3);
SPI_WriteData(0xC6);
SPI_WriteData(0x3D);
SPI_WriteData(0x00);

//===================================================Video Mode Initial Code
SSD2828_WritePackageSize(2);
SPI_WriteData(0x11);        // Sleep-Out
SPI_WriteData(0x00);         
mdelay(520);

SSD2828_WritePackageSize(2);
SPI_WriteData(0x29);        // Display On
SPI_WriteData(0x00);       

mdelay(520);


////////////////SSD2828 HP/////////////////////////


 //SSD2825_Initial
SPI_WriteCmd(0xb7);
SPI_WriteData(0x50);
SPI_WriteData(0x00);   //Configuration Register

SPI_WriteCmd(0xb8);
SPI_WriteData(0x00);
SPI_WriteData(0x00);   //VC(Virtual ChannelID) Control Register

SPI_WriteCmd(0xb9);
SPI_WriteData(0x00);//1=PLL disable
SPI_WriteData(0x00);

SPI_WriteCmd(0xBA);//PLL=(TX_CLK/MS)*NS 8228=480M 4428=240M  061E=120M 4214=240M 821E=360M 8219=300M 8225=444M 8224=432
SPI_WriteData(0x28);//D7-0=NS(0x01 : NS=1)//2lan--0x1e  4lan-0x10   //0x4210  20191027
SPI_WriteData(0x82);//D15-14=PLL范围 00=62.5-125 01=126-250 10=251-500 11=501-1000  DB12-8=MS(01:MS=1)

SPI_WriteCmd(0xBB);//LP Clock Divider LP clock = 400MHz / LPD / 8 = 480 / 8/ 8 = 7.5MHz
SPI_WriteData(0x03);//D5-0=LPD=0x1 – Divide by 2  //0x08  20191027
SPI_WriteData(0x00);

SPI_WriteCmd(0xb9);
SPI_WriteData(0x01);//1=PLL disable
SPI_WriteData(0x00);//SPI_WriteData(0x00);

SPI_WriteCmd(0xc9);
SPI_WriteData(0x02);
SPI_WriteData(0x23);   //p1: HS-Data-zero  p2: HS-Data- prepare  --> 8031 issue
mdelay(100);

SPI_WriteCmd(0xCA);
SPI_WriteData(0x01);//CLK Prepare
SPI_WriteData(0x23);//Clk Zero

SPI_WriteCmd(0xCB); //local_write_reg(addr=0xCB,data=0x0510)
SPI_WriteData(0x10); //Clk Post
SPI_WriteData(0x05); //Clk Per

SPI_WriteCmd(0xCC); //local_write_reg(addr=0xCC,data=0x100A)
SPI_WriteData(0x05); //HS Trail
SPI_WriteData(0x10); //Clk Trail

SPI_WriteCmd(0xD0); 
SPI_WriteData(0x00);
SPI_WriteData(0x00);

//RGB interface configuration
SPI_WriteCmd(0xB1);
SPI_WriteData(HSPW_MIPI );//HSPW 7
SPI_WriteData(VSPW_MIPI );//VSPW 18

SPI_WriteCmd(0xB2);
SPI_WriteData(HBPD_MIPI );//HBPD 0x65=104
SPI_WriteData(VBPD_MIPI );//VBPD 1e=30 减小下移

SPI_WriteCmd(0xB3);
SPI_WriteData(HFPD_MIPI );//HFPD 8
SPI_WriteData(VFPD_MIPI );//VFPD 10

SPI_WriteCmd(0xB4);//Horizontal active period 720=02D0
SPI_WriteData(0xE0);//013F=319 02D0=720  01E0=480
SPI_WriteData(0x01);

SPI_WriteCmd(0xB5);//Vertical active period 1280=0500
SPI_WriteData(0x00);//01DF=479 0500=1280
SPI_WriteData(0x05);

SPI_WriteCmd(0xB6);//RGB CLK  16BPP=00 18BPP=01
SPI_WriteData(0x03);//D7=0 D6=0 D5=0  D1-0=11 – 24bpp  //0X03 20191027
//SPI_WriteData(0x00);//D7=0 D6=0 D5=0  D1-0=11 – 24bpp
//SPI_WriteData(0x20);//
SPI_WriteData(0x00);//D15=VS D14=HS D13=CLK D12-9=NC D8=0=Video with blanking packet. 00-F0
//MIPI lane configuration
SPI_WriteCmd(0xDE);//通道数
SPI_WriteData(0x03);//11=4LANE 10=3LANE 01=2LANE 00=1LANE
SPI_WriteData(0x00);

SPI_WriteCmd(0xD6);//  05=BGR  04=RGB
SPI_WriteData(0x04);//D0=0=RGB 1:BGR D1=1=Most significant byte sent first
SPI_WriteData(0x00);

SPI_WriteCmd(0xB7);
SPI_WriteData(0x4B);
SPI_WriteData(0x02);

SPI_WriteCmd(0x2C);



/////////////////////////////////////////////

}
