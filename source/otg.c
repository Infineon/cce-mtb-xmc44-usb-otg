/*********************************************************************************
* File Name        :   otg.c
*
* Description      :   This is the source code for the OTG applcation
*
* Related Document :   See README.md
*
*******************************************************************************
* Copyright 2024, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

/* MTB header file includes*/
#include "cybsp.h"

#include "cy_retarget_io.h"

/* OTG header file includes */
#include "USB_OTG.h"

/* emUSB-Device header file includes */
#include "USB.h"
#include "USB_CDC.h"

/* emUSB-Host header file includes */
#include "USBH.h"
#include "USBH_CDC.h"

/* FreeRTOS header file */
#include "FreeRTOS.h"
#include "task.h"

/***********************************************************************************
 *  Define configurables
 **********************************************************************************/
#define DELAY_TASK                  (100U)
#define USB_CONFIG_DELAY            (50U)
#define DELAY_ECHO_COMMUNICATION    (5000U)

/* Size for tasks stack */
#define USB_MAIN_TASK_MEMORY_REQ    (500U)
#define USB_ISR_TASK_MEMORY_REQ     (500U)

/*********************************************************************
*
*      Global Variables
*
**********************************************************************/
static USB_CDC_HANDLE usb_cdcHandle;
static char        temp_buffer[USB_FS_BULK_MAX_PACKET_SIZE];

static bool cdc_line_coding_is_updated = false;
static USB_CDC_LINE_CODING cdc_line_coding;

/* Information that is used during enumeration. */
static const USB_DEVICE_INFO usb_deviceInfo = {
    0x058B,                       /* VendorId    */
    0x027D,                       /* ProductId    */
    "Infineon Technologies",      /* VendorName   */
    "CDC Code Example",           /* ProductName  */
    "12345678"                    /* SerialNumber */
};

static void usb_device_notify(void* usb_context, uint8_t usb_index, USBH_DEVICE_EVENT usb_event);
static void usbh_task(void* arg);
static void usbh_isr_task(void* arg);
static void device_task(void);

static USBH_NOTIFICATION_HOOK usbh_cdc_notification;

static volatile char       device_ready;
static I8                  device_index;
static uint8_t             data_buffer[64U];

/*******************************************************************************
* Function Prototypes
********************************************************************************/
static void device_app(void);
static void host_app(void);


/***********************************************************************************
 *  Function Name: main_task
 ***********************************************************************************
 * Summary:
 * Select Host or Device mode based on connection
 *
 * Parameters:
 * arg - is not used in this function, is required by FreeRTOS
 * 
 * Return:
 * void
 *
 **********************************************************************************/
void main_task(void * arg)
{
    (void) arg;
    int otg_state;

    cy_rslt_t result;
    
    /* Initialize retarget-io to use the debug UART port */
    result = cy_retarget_io_init(CYBSP_DEBUG_UART_HW);

    /* retarget-io init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* \x1b[2J\x1b[;H - ANSI ESC sequence for clear screen */
    printf("\x1b[2J\x1b[;H");
    printf("******************"
        " XMC MCU: USB OTG application "
        "******************\r\n\n");

    for (;;)
    {
        USB_OTG_Init();
        USBH_Logf_Application("OTG detection started");

        for (;;)
        {
            otg_state = USB_OTG_GetSessionState();

            if (otg_state != USB_OTG_ID_PIN_STATE_IS_INVALID)
            {
                break;
            }
            else
            {
                XMC_Delay(USB_CONFIG_DELAY);
                XMC_GPIO_SetOutputHigh(CYBSP_USER_LED1_PORT, CYBSP_USER_LED1_PIN);
                XMC_Delay(USB_CONFIG_DELAY);
                XMC_GPIO_SetOutputLow(CYBSP_USER_LED1_PORT, CYBSP_USER_LED1_PIN);
            }
        }

        USB_OTG_DeInit();
        XMC_Delay(USB_CONFIG_DELAY);

        if (otg_state == USB_OTG_ID_PIN_STATE_IS_HOST)
        {
            USBH_Logf_Application("Host session detected");
            host_app();
        }
        else if (otg_state == USB_OTG_ID_PIN_STATE_IS_DEVICE)
        {
            USBH_Logf_Application("Device session detected");
            device_app();
        }
        else
        {
            /* Do nothing. Under normal circumstances, the application shall never go to this condition */
            USBH_Logf_Application("Error! Incorrect state. Stop execution");
            for (;;);
        }

        XMC_Delay(USB_CONFIG_DELAY);
    }
}

/*********************************************************************
* Function Name: on_line_coding
**********************************************************************
* Summary:
*  Called whenever a "SetLineCoding" Packet has been received.
*  This function is called directly from an ISR in most cases.
*
* Parameters:
*  pLineCoding
*
* Return:
*  void
**********************************************************************/
static void on_line_coding(USB_CDC_LINE_CODING * pLineCoding)
{
    cdc_line_coding.DTERate     = pLineCoding->DTERate;
    cdc_line_coding.CharFormat  = pLineCoding->CharFormat;
    cdc_line_coding.ParityType  = pLineCoding->ParityType;
    cdc_line_coding.DataBits    = pLineCoding->DataBits;

    cdc_line_coding_is_updated = true;
}

/*********************************************************************
* Function Name: usb_add_cdc
**********************************************************************
* Summary:
*  Add communication device class to USB stack
*
* Parameters:
*  void
*
* Return:
*  void
**********************************************************************/
static void usb_add_cdc(void)
{
    static uint8_t OutBuffer[USB_FS_BULK_MAX_PACKET_SIZE];
    USB_CDC_INIT_DATA     InitData;
    USB_ADD_EP_INFO       EPBulkIn;
    USB_ADD_EP_INFO       EPBulkOut;
    USB_ADD_EP_INFO       EPIntIn;

    memset(&InitData, 0, sizeof(InitData));
    EPBulkIn.Flags          = 0;                             /* Flags not used */
    EPBulkIn.InDir          = USB_DIR_IN;                    /* IN direction (Device to Host) */
    EPBulkIn.Interval       = 0;                             /* Interval not used for Bulk endpoints */
    EPBulkIn.MaxPacketSize  = USB_FS_BULK_MAX_PACKET_SIZE;   /* Maximum packet size (64B for Bulk in full-speed) */
    EPBulkIn.TransferType   = USB_TRANSFER_TYPE_BULK;        /* Endpoint type - Bulk */
    InitData.EPIn  = USBD_AddEPEx(&EPBulkIn, NULL, 0);

    EPBulkOut.Flags         = 0;                             /* Flags not used */
    EPBulkOut.InDir         = USB_DIR_OUT;                   /* OUT direction (Host to Device) */
    EPBulkOut.Interval      = 0;                             /* Interval not used for Bulk endpoints */
    EPBulkOut.MaxPacketSize = USB_FS_BULK_MAX_PACKET_SIZE;   /* Maximum packet size (64B for Bulk in full-speed) */
    EPBulkOut.TransferType  = USB_TRANSFER_TYPE_BULK;        /* Endpoint type - Bulk */
    InitData.EPOut = USBD_AddEPEx(&EPBulkOut, OutBuffer, sizeof(OutBuffer));

    EPIntIn.Flags           = 0;                             /* Flags not used */
    EPIntIn.InDir           = USB_DIR_IN;                    /* IN direction (Device to Host) */
    EPIntIn.Interval        = 64;                            /* Interval of 8 ms (64 * 125us) */
    EPIntIn.MaxPacketSize   = USB_FS_INT_MAX_PACKET_SIZE ;   /* Maximum packet size (64 for Interrupt) */
    EPIntIn.TransferType    = USB_TRANSFER_TYPE_INT;         /* Endpoint type - Interrupt */
    InitData.EPInt = USBD_AddEPEx(&EPIntIn, NULL, 0);

    usb_cdcHandle = USBD_CDC_Add(&InitData);
    USBD_CDC_SetOnLineCoding(usb_cdcHandle, on_line_coding);
}

/***********************************************************************************
 *  Function Name: device_app
 ***********************************************************************************
 * Summary:
 * Configures the CDC device, waits for enumeration, and echoes all received data.
 * As soon as a disconnection event occurs, the function deinitializes emUSB-Device
 * and returns.
 *
 * Parameters:
 * None
 * 
 * Return:
 * void
 *
 **********************************************************************************/
static void device_app(void)
{
    int  num_bytes_received;

    /* Initializes the USB stack */
    USBD_Init();

    /* Endpoint Initialization for CDC class */
    usb_add_cdc();

    /* Set device info used in enumeration */
    USBD_SetDeviceInfo(&usb_deviceInfo);

    /* Start the USB stack */
    USBD_Start();

    USBH_Logf_Application("emUSB-Device is initialized");

    /* Wait for configuration */
    while ((USBD_GetState() & (USB_STAT_CONFIGURED | USB_STAT_SUSPENDED)) != USB_STAT_CONFIGURED)
    {
        XMC_GPIO_ToggleOutput(CYBSP_USER_LED1_PORT, CYBSP_USER_LED1_PIN);
        XMC_Delay(USB_CONFIG_DELAY);
    }

    USBH_Logf_Application("Device enumerated");
    USBH_Logf_Application("Please open another serial monitor for USB CDC Device");
    USBH_Logf_Application("Send any message to device and be sure that you receive it back.");

    for(;;)
    {
        int dev_state = USBD_GetState();

        /* Check disconnection event */
        if (((dev_state & USB_STAT_CONFIGURED) == 0U || (dev_state & USB_STAT_SUSPENDED) != 0U))
        {
            XMC_GPIO_SetOutputLow(CYBSP_USER_LED1_PORT, CYBSP_USER_LED1_PIN);
            USBH_Logf_Application("Device is disconnected");
            break;
        }

        XMC_GPIO_SetOutputHigh(CYBSP_USER_LED1_PORT, CYBSP_USER_LED1_PIN);

        if (cdc_line_coding_is_updated)
        {
            cdc_line_coding_is_updated = false;
            USBH_Logf_Application("DTERate=%lu, CharFormat=%u, ParityType=%u, DataBits=%u\n",
                                    cdc_line_coding.DTERate, cdc_line_coding.CharFormat,
                                    cdc_line_coding.ParityType, cdc_line_coding.DataBits);
        }

        memset (temp_buffer, 0, sizeof(temp_buffer));

        /* Receive one USB data packet and echo it back. */
        num_bytes_received = USBD_CDC_Receive(usb_cdcHandle, &temp_buffer[0], sizeof(temp_buffer), 0);
        
        USBH_Logf_Application("CDC data received from Host: %s", (char*) temp_buffer);

        if (num_bytes_received > 0)
        {
            USBD_CDC_Write(usb_cdcHandle, &temp_buffer[0], num_bytes_received, 0);
        }
        USBH_Logf_Application("CDC data sent to Host: %s", (char*) temp_buffer);
    }
}

/***********************************************************************************
 *  Function Name: host_app
 ***********************************************************************************
 * Summary:
 * Initializes emUSB-Host stack and registers all necessary tasks.
 * After disconnection of the device, deinitializes the emUSB-Host.
 *
 * Parameters:
 * None
 * 
 * Return:
 * void
 *
 **********************************************************************************/
static void host_app(void)
{
    USBH_STATUS usb_status;
    BaseType_t rtos_task_status;

    /* Initialize USBH stack */
    USBH_Init();

    /* Create two tasks mandatory for USBH operation */

    USBH_Logf_Application("Register usbh_task task \r\n");
    rtos_task_status = xTaskCreate(usbh_task, "usbh_task",
                                   USB_MAIN_TASK_MEMORY_REQ, NULL, configMAX_PRIORITIES - 2, NULL);

    if (rtos_task_status != pdPASS)
    {
        CY_ASSERT(0);
    }

    USBH_Logf_Application("Register usbh_isr_task task \r\n");

    rtos_task_status = xTaskCreate(usbh_isr_task, "usbh_isr_task",
                                   USB_ISR_TASK_MEMORY_REQ, NULL, configMAX_PRIORITIES - 1, NULL);
    if (rtos_task_status != pdPASS)
    {
        CY_ASSERT(0);
    }

    USBH_Logf_Application("Initialize CDC classes \r\n");

    /* Initialize CDC classes */
    USBH_CDC_Init();

    USBH_CDC_SetConfigFlags(USBH_CDC_IGNORE_INT_EP | USBH_CDC_DISABLE_INTERFACE_CHECK);
    usb_status = USBH_CDC_AddNotification(&usbh_cdc_notification, usb_device_notify, NULL);

    if(usb_status != USBH_STATUS_SUCCESS)
    {
        CY_ASSERT(0);
    }

    USBH_Logf_Application("Waiting for a USB CDC device \r\n\n");

    uint32_t wait_counter = 10U;

    for (;;)
    {
        USBH_OS_Delay(100U);

        if (device_ready)
        {
            wait_counter = 0U;
            device_task();
        }

        /* Check whether all devices were removed. */
        if (USB_OTG_GetIdPin() != 0 && USBH_GetNumRootPortConnections(0) == 0)
        {
            if (wait_counter == 0)
            {
                break;
            }
            wait_counter--;
        }
    }
}

/***********************************************************************************
 *  Function Name: usbh_task
 ***********************************************************************************
 * Summary:
 * Wrapper of USBH_Task() for FreeRTOS.
 *
 * Parameters:
 * arg - is not used in this function, is required by FreeRTOS
 * 
 * Return:
 * void
 *
 **********************************************************************************/
static void usbh_task(void* arg)
{
    (void)arg;

    USBH_Task();

    USBH_Logf_Application("usbh_task was released");
    
    /* Delete the task as soon as emUSB-Host is released */
    vTaskDelete(NULL);
}

/***********************************************************************************
 *   Function Name: usbh_isr_task
 ***********************************************************************************
 * Summary:
 * Wrapper of USBH_ISRTask() for FreeRTOS. 
 *
 * Parameters:
 * arg - is not used in this function, is required by FreeRTOS
 * 
 * Return:
 * void
 *
 **********************************************************************************/
static void usbh_isr_task(void* arg)
{
    (void)arg;

    USBH_ISRTask();

    USBH_Logf_Application("usbh_isr_task was released");
    
    /* Delete the task as soon as emUSB-Host is released */
    vTaskDelete(NULL);
}

/***********************************************************************************
 * Function Name: usb_device_notify
 ***********************************************************************************
 * Summary:
 * This function registers the addition or removal of devices from the host.
 * 
 * Parameters:
 * usb_context  :   Pointer to a context passed by the user in the call to one of
 *                  the register functions.
 * usb_index    :   Zero based index of the device that was added or removed.
 *                  First device has index 0, second one has index 1, etc
 * usb_event    :   Enum USBH_DEVICE_EVENT which gives information about the 
 *                  event that occurred.
 * 
 * Return:
 * void
 *
 **********************************************************************************/
static void usb_device_notify(void* usb_context, uint8_t usb_index, USBH_DEVICE_EVENT usb_event)
{
    (void)usb_context;

    switch (usb_event)
    {
        case USBH_DEVICE_EVENT_ADD:
            USBH_Logf_Application("======================== Device added [%d]" 
                                  "========================\n\n\n\n", usb_index);
            device_index = usb_index;
            device_ready = 1;
            break;

        case USBH_DEVICE_EVENT_REMOVE:
            USBH_Logf_Application("======================== Device removed [%d]" 
                                  "========================\n\n\n\n", usb_index);
            device_ready = 0;
            device_index   = -1;
            break;

        default:
            USBH_Logf_Application("======================== Invalid event [%d]" 
                                  "========================\n\n\n\n", usb_index);
            break;
    }
}

/***********************************************************************************
 * Function Name: device_task
 ***********************************************************************************
 * Summary:
 * This function is responsible for retreival of the device information and 
 * configures the CDC device to start the echo communication. It contains the 
 * print logs for the data packets as well as the echo data stream.
 * 
 * Parameters:
 * void
 * 
 * Return:
 * void
 *
 **********************************************************************************/
static void device_task(void)
{
    /* Open the device, the device index is retrieved from the notification callback. */
    USBH_CDC_HANDLE      device_handle = USBH_CDC_Open(device_index);

    if (device_handle)
    {
        USBH_CDC_DEVICE_INFO usb_device_info;
        USBH_STATUS          usb_status;
        unsigned long numBytes;

        /* Configure the CDC device. */
        USBH_CDC_SetTimeouts(device_handle, 50, 50);
        USBH_CDC_AllowShortRead(device_handle, 1);
        USBH_CDC_SetCommParas(device_handle, USBH_CDC_BAUD_115200, USBH_CDC_BITS_8,
                                USBH_CDC_STOP_BITS_1, USBH_CDC_PARITY_NONE);

        /* Retrieve the information about the CDC device */
        USBH_CDC_GetDeviceInfo(device_handle, &usb_device_info);
        USBH_Logf_Application("Vendor  ID = 0x%.4X\n", usb_device_info.VendorId);
        USBH_Logf_Application("Product ID = 0x%.4X\n", usb_device_info.ProductId);

        USBH_Logf_Application("Writing to the device \"Hello Infineon!\"\n");
        USBH_CDC_Write(device_handle, (const uint8_t *)"Hello Infineon!\n", 16U, &numBytes);
        USBH_Logf_Application("Reading from the device\n");
        usb_status = USBH_CDC_Read(device_handle, data_buffer, sizeof(data_buffer), &numBytes);

        if (usb_status != USBH_STATUS_SUCCESS)
        {
            USBH_Logf_Application("Error occurred during reading from device");
        }
        else
        {
            data_buffer[numBytes] = 0;
            USBH_Logf_Application("Received: %s \n",(char *)data_buffer);
            USBH_Logf_Application("Communication of USB Host with USB Device Successful\n");
        }

        USBH_Logf_Application("Re-initiating echo communication in 5 seconds\n\n\n\n");
        XMC_Delay(DELAY_ECHO_COMMUNICATION);
    }
}
