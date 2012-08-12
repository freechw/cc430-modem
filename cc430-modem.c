/******************************************************************************
 * Snowcap Radio Board v1
 * Act as a modem to Snowcap Control Board
 *
 * This code reads messages (a string ending with '\n') from UART and
 * sends them to RF and vise versa. Messages are always sent and
 * received as a whole over the RF and thus always buffered fully
 * before sending them in either direction.
 *
 * Code is modified from:
 * CC430 RF Code Example - TX and RX (variable packet length =< FIFO size)
 ******************************************************************************/
#include "cc430-modem.h"

#define PAYLOAD_LEN        (32)                // Max payload
#define PACKET_LEN         (PAYLOAD_LEN + 3)   // PACKET_LEN = payload + len + RSSI + LQI
#define UART_BUF_LEN       (PAYLOAD_LEN*3)     // Bigger buffers for uart (MAX: 127)
#define CRC_OK             (BIT7)              // CRC_OK bit
#define PATABLE_VAL        (0x51)              // 0 dBm output

#define UART_TXRX_SWAP                         // Swap TX & RX
#define USE_DEBUG_LEDS     (2)                 // 1 debug led
#define SC_USE_SLEEP       (1)                 // Use sleep instead busyloop

#ifdef USE_DEBUG_LEDS
#if (USE_DEBUG_LEDS >= 1)
#define DEBUG_LED1_POUT     P1OUT
#define DEBUG_LED1_PDIR     P1DIR
#define DEBUG_LED1_BIT      BIT0               // P1.0, GPIO in RBv1
#endif
#if (USE_DEBUG_LEDS == 2)
#define DEBUG_LED2_POUT     P1OUT
#define DEBUG_LED2_PDIR     P1DIR
#define DEBUG_LED2_BIT      BIT4               // P1.4, SPI in RBv1
#endif
#endif

#define UART_RX_NEWDATA_TIMEOUT_MS       4     // 4ms timeout for sending current uart rx data

#define CC430_STATE_TX                   (0x20)
#define CC430_STATE_IDLE                 (0x00)
#define CC430_STATE_TX_UNDERFLOW         (0x70)
#define CC430_STATE_MASK                 (0x70)
#define CC430_FIFO_BYTES_AVAILABLE_MASK  (0x0F)
#define CC430_STATE_RX                   (0x10)
#define CC430_STATE_RX_OVERFLOW          (0x60)


extern RF_SETTINGS rfSettings;

unsigned char packetReceived;
unsigned char packetTransmit;

// Buffer for incoming data from UART
unsigned char UartRxBuffer[UART_BUF_LEN];
unsigned char UartRxBuffer_i = 0;

// Buffer for outoing data over UART
unsigned char UartTxBuffer[UART_BUF_LEN];
unsigned char UartTxBuffer_i = 0;
unsigned char UartTxBufferLength = 0;
unsigned char uart_rx_timeout = 0;

// Buffer for incoming data from RF
unsigned char RfRxBuffer[PACKET_LEN];
unsigned char RfRxBufferLength = 0;

// Buffer for outgoing data over RF
unsigned char RfTxBuffer[PACKET_LEN];
unsigned char RfTxBuffer_i = 1;
unsigned char data_to_send = 0;
unsigned char rf_error = 0;

unsigned char rf_transmitting = 0;
unsigned char rf_receiving = 0;

int main(void)
{
  // Stop watchdog timer to prevent time out reset
  WDTCTL = WDTPW + WDTHOLD;

  // Increase PMMCOREV level to 2 for proper radio operation
  SetVCore(2);

  ResetRadioCore();
  InitUart();
  InitRadio();
  InitLeds();

  // Enable interrupts
  __bis_status_register(GIE);

  while (1) {

    led_toggle(1);

    // If not sending nor listening, start listening
    if(!rf_transmitting && !rf_receiving) {
      unsigned char RxStatus;

      RfReceiveOff();

      // Reset radio on error
      if (rf_error) {
        ResetRadioCore();
        InitRadio();
        rf_error = 0;
      }

      // Wait until idle
      while (((RxStatus = Strobe(RF_SNOP)) & CC430_STATE_MASK) != CC430_STATE_IDLE) {
        sleep_ms(1);
      }

      // Start listening
      rf_receiving = 1;
      RfReceiveOn();
    }

#if SC_USE_SLEEP == 1
    // Sleep while waiting for interrupt
    __bis_status_register(LPM0_bits + GIE);
#else
    sleep_ms(1);
#endif

    // We have data to send over RF
    if (!rf_transmitting && data_to_send) {
      if (!send_next_msg()) {
        continue;
      }
    }
  }

  return 0;
}



int send_next_msg(void)
{
  signed char x;
  signed char len = -1;

  // Disable interrupts to make sure UartRxBuffer isn't modified in the middle
  __bic_status_register(GIE);

  // On UART RX timeout, send all data in UART RX buffer
  if (uart_rx_timeout == 1) {
    len = UartRxBuffer_i;
    uart_rx_timeout = 0;
  } else {
    // Find the end of the first message
    for (x = 0; x < data_to_send; x++) {
      if (UartRxBuffer[x] == '\n') {
        len = x + 1;
        break;
      }
    }
  }

  // ERROR: no newline or empty uart buffers, do nothing
  if (len == 0 || len == -1) {
    data_to_send = 0;
    UartRxBuffer_i = 0;
    // Enable interrupts
    __bis_status_register(GIE);
    return 1;
  }

  // Radio expects first byte to be packet len (excluding the len byte itself)
  RfTxBuffer[0] = len;
  // Copy data received over uart to RF TX buffer
  for (x = 0; x < len; ++x) {
    RfTxBuffer[x+1] = UartRxBuffer[x];
  }

  // If we have more data to send, move them in front of the buffer
  if (len != UartRxBuffer_i) {
    for (x = 0; x < UartRxBuffer_i - len; ++x) {
      UartRxBuffer[x] = UartRxBuffer[x+len];
    }
    UartRxBuffer_i -= len;
  } else {
    // All data sent, clear uart buffers
    UartRxBuffer_i = 0;
    data_to_send = 0;
  }

  // Stop receive mode
  if (rf_receiving) {
    RfReceiveOff();
    rf_receiving = 0;
  }

  // Send buffer over RF (len +1 for length byte)
  rf_transmitting = 1;
  RfTransmit((unsigned char*)RfTxBuffer, RfTxBuffer[0] + 1);
  led_on(2);

  // Enable interrupts
  __bis_status_register(GIE);

  return 0;
}



/*
 * Toggle the specified led between on/off
 */
void led_toggle(int led)
{
#ifdef USE_DEBUG_LEDS

  switch(led) {
  case 1:
#if (USE_DEBUG_LEDS >= 1)
    DEBUG_LED1_POUT ^= DEBUG_LED1_BIT;
    break;
#endif
  case 2:
#if (USE_DEBUG_LEDS == 2)
    DEBUG_LED2_POUT ^= DEBUG_LED2_BIT;
#endif
    break;
  default:
    break;
  }
#endif
}



/*
 * Turn the specified led off
 */
void led_off(int led)
{
#ifdef USE_DEBUG_LEDS

  switch(led) {
  case 1:
#if (USE_DEBUG_LEDS >= 1)
    DEBUG_LED1_POUT &= ~DEBUG_LED1_BIT;
#endif
    break;
  case 2:
#if (USE_DEBUG_LEDS == 2)
    DEBUG_LED2_POUT &= ~DEBUG_LED2_BIT;
#endif
    break;
  default:
    break;
  }
#endif
}



/*
 * Turn the specified led on
 */
void led_on(int led)
{
#ifdef USE_DEBUG_LEDS

  switch(led) {
  case 1:
#if (USE_DEBUG_LEDS >= 1)
    DEBUG_LED1_POUT |= DEBUG_LED1_BIT;
#endif
    break;
  case 2:
#if (USE_DEBUG_LEDS == 2)
    DEBUG_LED2_POUT |= DEBUG_LED2_BIT;
#endif
    break;
  default:
    break;
  }
#endif
}


/*
 * Initialize JTAG (no leds in Radio Board v1)
 */
void InitLeds(void)
{
  // Initialize Port J
  PJOUT = 0x00;
  PJDIR = 0xFF;

  // Set up LEDs (no leds in Radio Board v1)
#if (USE_DEBUG_LEDS >= 1)
  // P1.0 (GPIO in Radio Board v1)
  DEBUG_LED1_POUT &= ~DEBUG_LED1_BIT;
  DEBUG_LED1_PDIR |= DEBUG_LED1_BIT;
#endif
#if (USE_DEBUG_LEDS == 2)
  // P1.2 (SPI in Radio Board v1)
  DEBUG_LED2_POUT &= ~DEBUG_LED2_BIT;
  DEBUG_LED2_PDIR |= DEBUG_LED2_BIT;
#endif
}



/*
 * Initialize CC1101 radio inside the CC430.
 */
void InitRadio(void)
{
  // Set the High-Power Mode Request Enable bit so LPM3 can be entered
  // with active radio enabled
  PMMCTL0_H = 0xA5;
  PMMCTL0_L |= PMMHPMRE_L;
  PMMCTL0_H = 0x00;

  WriteRfSettings(&rfSettings);

  WriteSinglePATable(PATABLE_VAL);
}



/*
 * Map P1.5 & P1.6 to Uart TX and RX and initialise Uart as 115200 8N1
 * with interrupts enabled
 */
void InitUart(void)
{
  PMAPPWD = 0x02D52;                        // Get write-access to port mapping regs
#ifdef UART_TXRX_SWAP
  P1MAP6 = PM_UCA0RXD;                      // Map UCA0RXD output to P1.6
  P1MAP5 = PM_UCA0TXD;                      // Map UCA0TXD output to P1.5
#else
  P1MAP5 = PM_UCA0RXD;                      // Map UCA0RXD output to P1.6
  P1MAP6 = PM_UCA0TXD;                      // Map UCA0TXD output to P1.5
#endif
  PMAPPWD = 0;                              // Lock port mapping registers

#ifdef UART_TXRX_SWAP
  P1DIR |= BIT5;                            // Set P1.5 as TX output
  P1SEL |= BIT5 + BIT6;                     // Select P1.5 & P1.6 to UART function
#else
  P1DIR |= BIT6;                            // Set P1.6 as TX output
  P1SEL |= BIT5 + BIT6;                     // Select P1.5 & P1.6 to UART function
#endif

  UCA0CTL1 |= UCSWRST;                      // **Put state machine in reset**
  UCA0CTL1 |= UCSSEL_2;                     // SMCLK
  UCA0BR0 = 9;                              // 1MHz 115200 (see User's Guide)
  UCA0BR1 = 0;                              // 1MHz 115200
  UCA0MCTL |= UCBRS_1 + UCBRF_0;            // Modulation UCBRSx=1, UCBRFx=0
  UCA0CTL1 &= ~UCSWRST;                     // **Initialize USCI state machine**
  UCA0IE |= UCRXIE;                         // Enable USCI_A0 RX interrupt
  UCA0IE |= UCTXIE;                         // Enable USCI_A0 TX interrupt
}



/*
 * Start RF transmit with the given message
 */
void RfTransmit(unsigned char *buffer, unsigned char length)
{
  // Falling edge of RFIFG9
  RF1AIES |= BIT9;

  // Clear pending interrupts
  RF1AIFG &= ~BIT9;

  // Enable TX end-of-packet interrupt
  RF1AIE |= BIT9;

  WriteBurstReg(RF_TXFIFOWR, buffer, length);

  // Start transmit
  Strobe(RF_STX);
}



/*
 * Turn on RF receiver
 */
void RfReceiveOn(void)
{  
  // Falling edge of RFIFG9
  RF1AIES |= BIT9;

  // Clear a pending interrupt
  RF1AIFG &= ~BIT9;

  // Enable the interrupt
  RF1AIE  |= BIT9;

  // Radio is in IDLE following a TX, so strobe SRX to enter Receive Mode
  Strobe(RF_SRX);
}



/*
 * Turn off RF receiver
 */
void RfReceiveOff(void)
{
  // Disable RX interrupts
  RF1AIE &= ~BIT9;

  // Clear pending IFG
  RF1AIFG &= ~BIT9;

  // It is possible that ReceiveOff is called while radio is receiving a packet.
  // Therefore, it is necessary to flush the RX FIFO after issuing IDLE strobe
  // such that the RXFIFO is empty prior to receiving a packet.
  Strobe(RF_SIDLE);
  Strobe(RF_SFRX);
}



/*
 * Uart TX or RX ready (one byte)
 */
__attribute__((interrupt(USCI_A0_VECTOR)))
void USCI_A0_ISR(void)
{

  switch(UCA0IV) {
  case 0: break;                            // Vector 0 - no interrupt
  case 2:                                   // Vector 2 - RXIFG
    if (handle_uart_rx_byte()) {
#if SC_USE_SLEEP == 1
      // Exit active
      __bic_SR_register_on_exit(LPM3_bits);
#endif
    }
    break;
  case 4:                                   // Vector 4 - TXIFG
    if (UartTxBufferLength == 0) {          // Spurious interrupt (or a workaround for a bug)?
      return;
    }

    if (++UartTxBuffer_i == UartTxBufferLength) { // All data sent?
      UartTxBuffer_i = 0;                   // Clear the Uart TX buffers
      UartTxBufferLength = 0;
      return;
    }

    // More data to be sent to Uart
    //while (!(UCA0IFG&UCTXIFG));             // USCI_A0 TX buffer ready? (should be always in here??)
    UCA0TXBUF = UartTxBuffer[UartTxBuffer_i]; // Send a byte

    break;
  default: break;
  }
}

/*
 * Called from interrupt handler to handle the received byte over uart
 */
int handle_uart_rx_byte(void)
{
  unsigned char tmpchar;

  // Clear possible pending timer for new data
  timer_clear();

  tmpchar = UCA0RXBUF;

  // Discard the byte if buffer already full
  if (UartRxBuffer_i == UART_BUF_LEN) {
    return 0;
  }

  // Store received byte
  UartRxBuffer[UartRxBuffer_i++] = tmpchar;

  // Check for a full message
  if (tmpchar == '\n') {

    // Flag ready to send over RF
    data_to_send = UartRxBuffer_i;

    return 1;
  }

  // Set timer for new data. Send current data after timeout
  timer_set(UART_RX_NEWDATA_TIMEOUT_MS);

  return 0;
}

/*
 * RF TX or RX ready (one whole message)
 */
__attribute__((interrupt(CC1101_VECTOR)))
void CC1101_ISR(void)
{
  switch(RF1AIV) {                          // Prioritizing Radio Core Interrupt
  case  0: break;                           // No RF core interrupt pending
  case  2: break;                           // RFIFG0
  case  4: break;                           // RFIFG1
  case  6: break;                           // RFIFG2
  case  8: break;                           // RFIFG3
  case 10: break;                           // RFIFG4
  case 12: break;                           // RFIFG5
  case 14: break;                           // RFIFG6
  case 16: break;                           // RFIFG7
  case 18: break;                           // RFIFG8
  case 20:                                  // RFIFG9

    // Disable RFIFG9 interrupts
    RF1AIE &= ~BIT9;

    // RX end of packet
    if(rf_receiving) {
      handle_rf_rx_packet();
    }

    // RF TX end of packet
    if(rf_transmitting) {
      led_off(2);
      rf_transmitting = 0;
    }
    break;
  case 22: break;                           // RFIFG10
  case 24: break;                           // RFIFG11
  case 26: break;                           // RFIFG12
  case 28: break;                           // RFIFG13
  case 30: break;                           // RFIFG14
  case 32: break;                           // RFIFG15
  }

#if SC_USE_SLEEP == 1
  __bic_SR_register_on_exit(LPM3_bits);     // Exit active
#endif
}



/*
 * Called from interrupt handler to handle a received packet from radio
 */
void handle_rf_rx_packet(void)
{
  unsigned char x;
  unsigned char RxStatus;

  // Radio is in IDLE after receiving a message (See MCSM0 default values)
  rf_receiving = 0;

  // Validate radio state
  RxStatus = Strobe(RF_SNOP);
  if ((RxStatus & CC430_STATE_MASK) != CC430_STATE_IDLE) {
    rf_error = 1;
    goto rx_error;
  }

  // Read the length byte from the FIFO
  RfRxBufferLength = ReadSingleReg(RXBYTES);

  // FIXME: assuming +\n is not generic
  // Must have at least 5 bytes (len <payload> \n RSSI CRC) for a valid packet
  if (RfRxBufferLength < 5) {
    goto rx_error;
  }

  // Read the packet data
  ReadBurstReg(RF_RXFIFORD, RfRxBuffer, RfRxBufferLength);

  // Verify CRC
  if(!(RfRxBuffer[RfRxBufferLength - 1] & CRC_OK)) {
    goto rx_error;
  }

  // If there's not enough space for new data in uart tx buffer, discard new data
  if (UartTxBufferLength + (RfRxBufferLength - 3) > UART_BUF_LEN) {
    goto failed_to_receive;
  }

  // Append the RF RX buffer to Uart TX, skipping the length, RSSI and CRC/Quality bytes
  for (x = 0; x < RfRxBufferLength - 3; ++x) {
    UartTxBuffer[UartTxBufferLength + x] = RfRxBuffer[x+1];
  }
  UartTxBufferLength += (RfRxBufferLength - 3);

  {
    // DEBUG: Write RSSI to UartTxBuffer
    char len;
    unsigned char *buf = &UartTxBuffer[UartTxBufferLength];
    unsigned char value = RfRxBuffer[RfRxBufferLength - 2];
    unsigned char max_len = UART_BUF_LEN - UartTxBufferLength;
    len = sc_itoa(value, buf, max_len);
    if (len == 0) {
      UartTxBuffer[UartTxBufferLength] = 'X';
      ++len;
    }
    UartTxBuffer[UartTxBufferLength + len++] = ' ';
    UartTxBufferLength += len;
  }

  {
    // DEBUG: Write CRC/LQI to UartTxBuffer
    char len;
    unsigned char *buf = &UartTxBuffer[UartTxBufferLength];
    unsigned char value = RfRxBuffer[RfRxBufferLength - 1];
    unsigned char max_len = UART_BUF_LEN - UartTxBufferLength;
    len = sc_itoa(value, buf, max_len);
    if (len == 0) {
      UartTxBuffer[UartTxBufferLength] = 'X';
      ++len;
    }
    UartTxBuffer[UartTxBufferLength + len++] = '\n';
    UartTxBufferLength += len;
  }

  // Send the first byte to Uart
  //while (!(UCA0IFG&UCTXIFG));           // USCI_A0 TX buffer ready?
  UCA0TXBUF = UartTxBuffer[UartTxBuffer_i]; // Send first byte
  return;

 rx_error:
  rf_error = 1;

 failed_to_receive:
  RfRxBufferLength = 0;
  return;
}

/*
 * Convert int to string. Returns length on the converted string
 * (excluding the trailing \0) or 0 in error.
 */
int sc_itoa(int value, unsigned char *str, int len)
{
  int index = len;
  int extra, i;
  int str_len;
  int negative = 0;
  int start = 0;

  // Check for negative values
  if (value < 0) {
	negative = 1;
	value *= -1;
  }

  // Go through all numbers and store the last digit to the string
  // starting for the last character
  while (--index >= 0) {
	str[index] = (value % 10) + '0';
	if (value < 10) {
	  break;
	}
	value /= 10;
  }

  // Check for overflow (needs also space for '\0' and possible '-').
  if (index < 1 || (negative && index < 2)) {
	str[0] = '\0';
	return 0;
  }

  // Add - for negative numbers
  if (negative) {
	str[0] = '-';
	start = 1;
  }

  // Move numbers to the start of the string. Add \0 after the last
  // number or as the last character in case of overflow
  extra = index;
  for (i = 0; i < len - extra; i++) {
	str[i + start] = str[i + extra];
  }
  str_len = len - extra + start;

  str[str_len] = '\0';

  return str_len;
}



/*
 * Sleep ms milliseconds.
 */
void sleep_ms(int ms)
{
  int a;
  for (a = 0; a < ms; a++) {
    __delay_cycles(1000);
  }
}



/*
 * Timeout, send data currently in the UART RX buffer
 */
__attribute__((interrupt(TIMER1_A0_VECTOR)))
void TIMER1_A0_ISR(void)
{
  timer_clear();
  if (UartRxBuffer_i > 0) {
    data_to_send = 1;
    uart_rx_timeout = 1;
  }
}



/*
 * Set timer to raise an interrupt after ms milliseconds
 * NOTE: ms must < 512
 */
void timer_set(int ms)
{
  if (ms > 195) {
    ms = 195;
  }

  TA1CCR0  = ms << 7;                       // ms milliseconds
  TA1CTL = TASSEL_2 + MC_1 + ID_3;          // SMCLK/8, upmode
  TA1CCTL0 = CCIE;                          // CCR0 interrupt enabled
}



/*
 * Set timer to raise an interrupt after ms milliseconds
 */
void timer_clear(void)
{
  // Clear timer register, disable timer interrupts
  TA1CTL = TACLR;
  uart_rx_timeout = 0;
}

/* Emacs indentatation information
   Local Variables:
   indent-tabs-mode:nil
   tab-width:2
   c-basic-offset:2
   End:
*/

