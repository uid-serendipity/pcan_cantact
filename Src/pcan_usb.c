#include <stm32f0xx_hal.h>
#include <assert.h>
#include "usbd_ctlreq.h"
#include "usbd_ioreq.h"
#include "usbd_conf.h"
#include "usbd_helper.h"
#include "pcan_protocol.h"

/* PCAN-USB Endpoints */
#define PCAN_USB_EP_CMDOUT  0x01
#define PCAN_USB_EP_CMDIN   0x81
#define PCAN_USB_EP_MSGOUT  0x02
#define PCAN_USB_EP_MSGIN   0x82

static uint8_t buffer_cmd[16];
static uint8_t buffer_data[64];
USBD_HandleTypeDef hUsbDeviceFS;
extern USBD_DescriptorsTypeDef FS_Desc;

struct t_pcan_description
{
  USB_CONFIGURATION_DESCRIPTOR con0;
  USB_INTERFACE_DESCRIPTOR     if0;
  USB_ENDPOINT_DESCRIPTOR      ep1;
  USB_ENDPOINT_DESCRIPTOR      ep2;
  USB_ENDPOINT_DESCRIPTOR      ep3;
  USB_ENDPOINT_DESCRIPTOR      ep4;
};

__ALIGN_BEGIN static const USB_DEVICE_QUALIFIER_DESCRIPTOR dev_qua __ALIGN_END = 
{
  .bLength            = sizeof( USB_DEVICE_QUALIFIER_DESCRIPTOR ),
  .bDescriptorType    = USB_QUALIFIER_DESCRIPTOR_TYPE,
  .bcdUSB             = 0x0100, /* 1.0 */
  .bDeviceClass       = 0,
  .bDeviceSubClass    = 0,
  .bDeviceProtocol    = 0,
  .bMaxPacketSize0    = 64,
  .bNumConfigurations = 1,
  .bReserved          = 0,
};

__ALIGN_BEGIN static struct t_pcan_description pcan_usb_dev __ALIGN_END = 
{
  .con0 =
  {
    .bLength              = sizeof( USB_CONFIGURATION_DESCRIPTOR ),
    .bDescriptorType      = USB_CONFIGURATION_DESCRIPTOR_TYPE,
    .wTotalLength         = sizeof( struct t_pcan_description ), 
    .bNumInterfaces       = 1,
    .bConfigurationValue  = 1,
    .iConfiguration       = 0,
    .bmAttributes         = USB_CONFIG_BUS_POWERED,
    .MaxPower             = 100, /* = 200mA */
  },
  .if0 =
  {
    .bLength              = sizeof( USB_INTERFACE_DESCRIPTOR ),
    .bDescriptorType      = USB_INTERFACE_DESCRIPTOR_TYPE,
    .bInterfaceNumber     = 0,
    .bAlternateSetting    = 0,
    .bNumEndpoints        = 4,
    .bInterfaceClass      = 0,
    .bInterfaceSubClass   = 0,
    .bInterfaceProtocol   = 0,
    .iInterface           = 0,
  },
  .ep1 = 
  {
    .bLength              = sizeof( USB_ENDPOINT_DESCRIPTOR ),
    .bDescriptorType      = USB_ENDPOINT_DESCRIPTOR_TYPE,
    .bEndpointAddress     = PCAN_USB_EP_CMDIN, /* PC IN cmd resp */
    .bmAttributes         = USB_ENDPOINT_TYPE_BULK,
    .wMaxPacketSize       = 16,
    .bInterval            = 0,
  },
  .ep2 = 
  {
    .bLength              = sizeof( USB_ENDPOINT_DESCRIPTOR ),
    .bDescriptorType      = USB_ENDPOINT_DESCRIPTOR_TYPE,
    .bEndpointAddress     = PCAN_USB_EP_CMDOUT, /* PC OUT cmd */
    .bmAttributes         = USB_ENDPOINT_TYPE_BULK,
    .wMaxPacketSize       = 16,
    .bInterval            = 0,
  },
  .ep3 = 
  {
    .bLength              = sizeof( USB_ENDPOINT_DESCRIPTOR ),
    .bDescriptorType      = USB_ENDPOINT_DESCRIPTOR_TYPE,
    .bEndpointAddress     = PCAN_USB_EP_MSGIN, /* PC IN frames */
    .bmAttributes         = USB_ENDPOINT_TYPE_BULK,
    .wMaxPacketSize       = 64,
    .bInterval            = 0,
  },
  .ep4 = 
  {
    .bLength              = sizeof( USB_ENDPOINT_DESCRIPTOR ),
    .bDescriptorType      = USB_ENDPOINT_DESCRIPTOR_TYPE,
    .bEndpointAddress     = PCAN_USB_EP_MSGOUT, 
    .bmAttributes         = USB_ENDPOINT_TYPE_BULK,/* PC OUT frames */
    .wMaxPacketSize       = 64,
    .bInterval            = 0,
  },
};


static uint8_t device_init( USBD_HandleTypeDef *pdev, uint8_t cfgidx )
{
  USB_ENDPOINT_DESCRIPTOR *p_ep = &pcan_usb_dev.ep1;

  UNUSED( cfgidx );
  
  for( int i = 0; i < pcan_usb_dev.if0.bNumEndpoints; i++ )
  {
    uint8_t ep_addr = p_ep[i].bEndpointAddress;
    
    if( p_ep[i].bmAttributes == USB_ENDPOINT_TYPE_BULK )
    {
      if( pdev->dev_speed == USBD_SPEED_FULL )
        ;
      else if( pdev->dev_speed == USBD_SPEED_HIGH )
        ;
      else
        assert( 0 );
    }
    
    USBD_LL_OpenEP( pdev, ep_addr,
                          p_ep[i].bmAttributes,
                          p_ep[i].wMaxPacketSize );
    
    if( ( ep_addr & 0x80 ) != 0 )
      pdev->ep_in[ep_addr & EP_ADDR_MSK].is_used = 1;
    else
      pdev->ep_out[ep_addr & EP_ADDR_MSK].is_used = 1;
  }
    
  pdev->pClassData = (void*)1;


  USBD_LL_PrepareReceive( pdev, PCAN_USB_EP_CMDOUT, buffer_cmd, sizeof( buffer_cmd ) );
  USBD_LL_PrepareReceive( pdev, PCAN_USB_EP_MSGOUT, buffer_data, sizeof( buffer_data ) );
  
  return USBD_OK;
}

static uint8_t device_deinit( USBD_HandleTypeDef *pdev, uint8_t cfgidx )
{
  USB_ENDPOINT_DESCRIPTOR const *p_ep = &pcan_usb_dev.ep1;

  UNUSED( cfgidx );
  
  for( int i = 0; i < pcan_usb_dev.if0.bNumEndpoints; i++ )
  {
    uint8_t ep_addr = p_ep[i].bEndpointAddress;
    USBD_LL_CloseEP( pdev, ep_addr );
    if( ( ep_addr & 0x80 ) != 0 )
      pdev->ep_in[ep_addr & EP_ADDR_MSK].is_used = 0;
    else
      pdev->ep_out[ep_addr & EP_ADDR_MSK].is_used = 0;
  }
  
  pdev->pClassData = (void*)0;
  return USBD_OK;
}

uint16_t pcan_usb_send_command_buffer( const void *p, uint16_t size )
{
  USBD_HandleTypeDef *pdev = &hUsbDeviceFS;

  if( pdev->ep_in[PCAN_USB_EP_CMDIN & 0xFU].total_length )
    return 0;
  pdev->ep_in[PCAN_USB_EP_CMDIN & 0xFU].total_length = size;
  
  if( USBD_LL_Transmit( pdev, PCAN_USB_EP_CMDIN, (void*)p, size ) == USBD_OK )
    return size;
  return 0;
}

uint16_t pcan_usb_send_data_buffer( const void *p, uint16_t size )
{
  USBD_HandleTypeDef *pdev = &hUsbDeviceFS;

  if( pdev->ep_in[PCAN_USB_EP_MSGIN & 0xFU].total_length )
    return 0;
  pdev->ep_in[PCAN_USB_EP_MSGIN & 0xFU].total_length = size;
  
  if( USBD_LL_Transmit( pdev, PCAN_USB_EP_MSGIN, (void*)p, size ) == USBD_OK )
    return size;
  return 0;
}

void pcan_usb_poll( void )
{
  HAL_PCD_IRQHandler( &hpcd_USB_FS );
}

static uint8_t  device_setup( USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req )
{
  switch( req->bRequest )
  {
    /* get info */
    case 0:
      switch( req->wValue )
      {
        case 0: /* bootloader info */
        {
          uint8_t bootloader_info[] = { 
                                  0x00, 0x00, 0x08, 0x04, 0x00, 0x08, 0x07, 0x00,
                                  0x04, 0x02, 0xe0, 0x07, 0x01, 0x00, 0x00, 0x00
                                };
          USBD_CtlSendData( pdev, bootloader_info, sizeof( bootloader_info ) );
        }
        break;
      }
    break;  
  }
  return USBD_OK;
}

static uint8_t device_ep0_rx_ready( USBD_HandleTypeDef *pdev )
{
  UNUSED( pdev );
  return USBD_OK;
}

/* data was sent to PC */
static uint8_t device_data_in( USBD_HandleTypeDef *pdev, uint8_t epnum )
{
  (void)pdev;
  (void)epnum;

  PCD_HandleTypeDef *hpcd = pdev->pData;

  if( pdev->pClassData == NULL )
    return USBD_FAIL;

  uint32_t len = pdev->ep_in[epnum].total_length;
  if( len && ( len % hpcd->IN_ep[epnum].maxpacket ) == 0U )
  {
    if( epnum == PCAN_USB_EP_CMDIN )
    {
      USBD_LL_Transmit( pdev, epnum, NULL, 0U );
    }
  }
  pdev->ep_in[epnum].total_length = 0U;

  return USBD_OK;
}

/* data was received from PC */
static uint8_t device_data_out( USBD_HandleTypeDef *pdev, uint8_t epnum )
{
  int size;
   
  if( pdev->pClassData == 0 )
    return USBD_FAIL;
  
  size = USBD_LL_GetRxDataSize( pdev, epnum );
  (void)size;

  if( epnum == PCAN_USB_EP_CMDOUT )
  {
    pcan_protocol_process_command( buffer_cmd, size );
    USBD_LL_PrepareReceive( pdev, PCAN_USB_EP_CMDOUT, buffer_cmd, sizeof( buffer_cmd ) );
  }
  else if( epnum == PCAN_USB_EP_MSGOUT )
  {
    pcan_protocol_process_data( buffer_data, size );
    USBD_LL_PrepareReceive( pdev, PCAN_USB_EP_MSGOUT, buffer_data, sizeof( buffer_data ) );
  }
  else
  {
    return USBD_FAIL;
  }

  return USBD_OK;
}

static uint8_t *device_get_hs_cfg( uint16_t *length )
{
  *length = sizeof( struct t_pcan_description );
  return (void*)&pcan_usb_dev;
}

static uint8_t *device_get_fs_cfg( uint16_t *length )
{
  *length = sizeof( struct t_pcan_description );
  return (void*)&pcan_usb_dev;
}

static uint8_t *device_get_other_speed_cfg( uint16_t *length )
{
  *length = sizeof( struct t_pcan_description );
  return (void*)&pcan_usb_dev;
}

static uint8_t *device_get_device_qualifier( uint16_t *length )
{
  *length = sizeof( USB_DEVICE_QUALIFIER_DESCRIPTOR );
  
  return (void*)&dev_qua;
}


static uint8_t *device_get_user_string( USBD_HandleTypeDef *pdev, uint8_t index, uint16_t *length )
{
  __ALIGN_BEGIN static uint8_t USBD_StrDesc[64] __ALIGN_END;

  UNUSED( pdev );

  switch( index )
  {
    case 10:
      USBD_GetString((uint8_t *)"PEAK-System Technik GmbH", USBD_StrDesc, length);
    break;
    default:
      return 0;
  }
  return USBD_StrDesc;
}

USBD_ClassTypeDef usbd_pcan =
{
  .Init = device_init,
  .DeInit = device_deinit,
  .Setup = device_setup,
  .EP0_TxSent = 0,
  .EP0_RxReady = device_ep0_rx_ready,
  .DataIn = device_data_in,
  .DataOut = device_data_out,
  .SOF = 0,
  .IsoINIncomplete = 0,
  .IsoOUTIncomplete = 0,
  .GetHSConfigDescriptor = device_get_hs_cfg,
  .GetFSConfigDescriptor = device_get_fs_cfg,
  .GetOtherSpeedConfigDescriptor = device_get_other_speed_cfg,
  .GetDeviceQualifierDescriptor = device_get_device_qualifier,
#if (USBD_SUPPORT_USER_STRING_DESC == 1U)
  .GetUsrStrDescriptor = device_get_user_string,
#endif
};

void pcan_usb_init( void )
{
  if( USBD_Init( &hUsbDeviceFS, &FS_Desc, DEVICE_FS ) != USBD_OK )
  {
    assert( 0 );
  }

  if( USBD_RegisterClass( &hUsbDeviceFS, &usbd_pcan ) != USBD_OK )
  {
    assert( 0 );
  }

  if( USBD_Start(&hUsbDeviceFS) != USBD_OK )
  {
    assert( 0 );
  }
}
