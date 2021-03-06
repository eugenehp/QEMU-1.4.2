/*
 * CAN device (SJA1000) simulation based on PCI-bus
 *
 * Copyright (c) 2013 Jin Yang
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifdef __linux__
#include "pci/pci.h"
#include "char/char.h"
#include "qemu/timer.h"
#include "exec/address-spaces.h"
#include <linux/types.h>

#include "can-pci.h"

//#define DEBUG_FILTER

static void can_software_reset(CanState *s)
{
	s->mode 		&= ~0x31;
	s->mode			|= 0x01;
	s->statusP 		&= ~0x37;
	s->statusP 		|= 0x34;

	s->rxbuf_start 	= 0x00;
	s->rxmsg_cnt	= 0x00;
	s->rx_cnt		= 0x00;
}

static void can_hardware_reset(void *opaque)
{
	CanState *s = opaque;

	// Reset by hardware, p10
	s->mode			= 0x01;
	s->statusP 		= 0x3c;
	s->interruptP	= 0x00;
	s->clock		= 0x00;
	s->rxbuf_start 	= 0x00;
	s->rxmsg_cnt	= 0x00;
	s->rx_cnt		= 0x00;

	s->control		= 0x01;
	s->statusB		= 0x0c;
	s->interruptB	= 0x00;

    qemu_mutex_init(&s->rx_lock);

	qemu_irq_lower(s->irq);
}


// Details in DS-p22, what we need to do here is to test the data.
static int accept_filter(CanState *s, struct can_frame *can)
{
	uint8_t tmp1, tmp2;

	if (s->clock & 0x80) { // PeliCAN Mode
		if(s->mode & (1 << 3)) { // Single mode.
			if(!(can->can_id & (1 << 31))) { // SFF
				if(can->can_id & (1 << 30)) // RTR
					return 1;
				if(can->can_dlc == 0)
					return 1;
				if(can->can_dlc == 1)
					if((can->data[0] & ~(s->code_mask[6])) == 
					   (s->code_mask[2] & ~(s->code_mask[6])))
						return 1;
				if(can->can_dlc >= 2)
					if(((can->data[0] & ~(s->code_mask[6])) == 
					   (s->code_mask[2] & ~(s->code_mask[6]))) &&
					   ((can->data[1] & ~(s->code_mask[7])) == 
					   (s->code_mask[3] & ~(s->code_mask[7]))))
						return 1;
				return 0;
			}
		} else { // Dual mode
			if(!(can->can_id & (1 << 31))) { // SFF
				if(((s->code_mask[0] & ~s->code_mask[4]) == 
					(((can->can_id >> 3) & 0xff) & ~s->code_mask[4])) &&
					(((s->code_mask[1] & ~s->code_mask[5]) & 0xe0) == 
					(((can->can_id << 5) & ~s->code_mask[5]) & 0xe0))) {
					if(can->can_dlc == 0)
						return 1;
					else {
						tmp1 = ((s->code_mask[1] << 4) & 0xf0) |
							  (s->code_mask[2] & 0x0f);
						tmp2 = ((s->code_mask[5] << 4) & 0xf0) |
							  (s->code_mask[6] & 0x0f);
						tmp2 = ~tmp2;
						if((tmp1 & tmp2) == (can->data[0] & tmp2))
							return 1;
						return 0;
					}
				}
			}
		}
	}

	return 1;
}

#ifdef DEBUG_FILTER
static void display_msg(struct can_frame *msg)
{
	int i;

	printf("%03X [%01d] -", (msg->can_id & 0x1fffffff), msg->can_dlc);
	if(msg->can_id & (1 << 31)) printf("EFF "); else printf("SFF ");
	if(msg->can_id & (1 << 30)) printf("RTR-"); else printf("DAT-");
	for(i = 0; i < msg->can_dlc; i++) {
		printf("  %02X", msg->data[i]);
	}
	for(; i < 8; i++)
		printf("    ");
	fflush(stdout);
}
#endif
static void buff2frameP(uint8_t *buff, struct can_frame *can)
{
	uint8_t i;

	can->can_id = 0;
	if (buff[0] & 0x40) // RTR
		can->can_id = 0x01 << 30;
	can->can_dlc = buff[0] & 0x0f;

	if (buff[0] & 0x80) { // Extended
		can->can_id |= 0x01 << 31;
		can->can_id |= buff[1] << 21; // ID.28~ID.21
		can->can_id |= buff[2] << 13; // ID.20~ID.13
		can->can_id |= buff[3] << 05;
		can->can_id |= buff[4] >> 03;
		for (i = 0; i < can->can_dlc; i++)
			can->data[i] = buff[5+i];
		for (; i < 8; i++)
			can->data[i] = 0;
	} else {
		can->can_id |= buff[1] << 03;
		can->can_id |= buff[2] >> 05;
		for (i = 0; i < can->can_dlc; i++)
			can->data[i] = buff[3+i];
		for (; i < 8; i++)
			can->data[i] = 0;
	}
}


static void buff2frameB(uint8_t *buff, struct can_frame *can)
{
	uint8_t i;

	can->can_id = ((buff[0] << 3) & (0xff << 3)) + ((buff[1] >> 5) & 0x07);
	if (buff[1] & 0x10) // RTR
		can->can_id = 0x01 << 30;
	can->can_dlc = buff[1] & 0x0f;

	for (i = 0; i < can->can_dlc; i++)
		can->data[i] = buff[2+i];
	for (; i < 8; i++)
		can->data[i] = 0;
}


static int frame2buffP(struct can_frame *can, uint8_t *buff)
{
	int i, count = 0;

	if(can->can_id & (1 << 29)) // error frame, NOT support now.
		return -1;

	buff[count] = 0x0f & can->can_dlc; // DLC
	if(can->can_id & (1 << 30)) // RTR
		buff[count] |= (1 << 6);
	if(can->can_id & (1 << 31)) { // EFF
		buff[count] |= (1 << 7);
		buff[++count] = (can->can_id >> 21) & 0xff; // ID.28~ID.21
		buff[++count] = (can->can_id >> 13) & 0xff; // ID.20~ID.13
		buff[++count] = (can->can_id >> 05) & 0xff; // ID.12~ID.05
		buff[++count] = (can->can_id << 03) & 0xf8; // ID.04~ID.00,x,x,x
		for (i = 0; i < can->can_dlc; i++) {
			buff[++count] = can->data[i];
		}

		return count + 1;
	} else { // SFF
		buff[++count] = (can->can_id >> 03) & 0xff; // ID.10~ID.03
		buff[++count] = (can->can_id << 05) & 0xe0; // ID.02~ID.00,x,x,x,x,x
		for (i = 0; i < can->can_dlc; i++) {
			buff[++count] = can->data[i];
		}

		return count + 1;
	}

	return -1;		
}

static int frame2buffB(struct can_frame *can, uint8_t *buff)
{
	int i, count = 0;

	if((can->can_id & (1 << 31)) || // EFF, not support for BasicMode.
	   (can->can_id & (1 << 29))) 	// or Error frame, NOT support now.
		return -1;

	
	buff[count++] = 0xff & (can->can_id >> 3);
	buff[count] = 0xe0 & (can->can_id << 5);
	if(can->can_id & (1 << 30)) // RTR
		buff[count] |= (1 << 4);
	buff[count++] |= can->can_dlc & 0x0f;
	for (i = 0; i < can->can_dlc; i++) {
		buff[count++] = can->data[i];
	}

#ifdef DEBUG_FILTER
	printf(" ==2==");
	for (i = 0; i < count; i++)
		printf(" %02X", buff[i]);
	for (; i < 10; i++)
		printf("   ");
#endif
	return count;	
}


static void can_mem_write(void *opaque, hwaddr addr, uint64_t val, unsigned size) 
{
    CanState 			*s = opaque;
	int    				i;
	struct can_frame	can;
	uint32_t			tmp;
	uint8_t				tmp8, count;


	DPRINTF("write 0x%llx addr(%d)\n", val, (int)addr);
 
	i  = (int)memory_region_size(&s->memio);
	if(addr > i)
		return ;

	if (s->clock & 0x80) { // PeliCAN Mode
		switch (addr) {
			case SJAMOD: // Mode register
				s->mode = 0x1f & val;
				if((s->mode & 0x01) && ((val & 0x01) == 0)) {
					// Go to operation mode from reset mode.
					if(s->mode & (1 << 3)) { // Single mode.
						// For EFF
						tmp = ((s->code_mask[0] << 21) & (0xff << 21)) |
							  ((s->code_mask[1] << 13) & (0xff << 13)) |
							  ((s->code_mask[2] <<  5) & (0xff <<  5)) |
							  ((s->code_mask[3] >>  3) & 0x1f) |
							  (1 << 31);
						s->filter[0].can_id = tmp;

						tmp = ((s->code_mask[4] << 21) & (0xff << 21)) |
							  ((s->code_mask[5] << 13) & (0xff << 13)) |
							  ((s->code_mask[6] <<  5) & (0xff <<  5)) |
							  ((s->code_mask[7] >>  3) & 0x1f) |
							  (7 << 29);
						s->filter[0].can_mask = ~tmp | (1 << 31);

						if(s->code_mask[3] & (1 << 2)) // RTR
							s->filter[0].can_id |= (1 << 30);
						if(!(s->code_mask[7] & (1 << 2)))
							s->filter[0].can_mask |= (1 << 30);

						// For SFF						
						tmp = ((s->code_mask[0] <<  3) & (0xff <<  3)) |
							  ((s->code_mask[1] >>  5) & 0x07);
						s->filter[1].can_id = tmp;

						tmp = ((s->code_mask[4] <<  3) & (0xff <<  3)) |
							  ((s->code_mask[5] >>  5) & 0x07) |
							  (0xff << 11) | (0xff << 19) | (0x0f << 27);
						s->filter[1].can_mask = ~tmp | (1 << 31);

						if(s->code_mask[1] & (1 << 4)) // RTR
							s->filter[1].can_id |= (1 << 30);
						if(!(s->code_mask[5] & (1 << 4)))
							s->filter[1].can_mask |= (1 << 30);

						qemu_chr_fe_ioctl(s->chr, 2, s->filter);
					} else { // Dual mode
						// For EFF
						tmp = ((s->code_mask[0] << 21) & (0xff << 21)) |
							  ((s->code_mask[1] << 13) & (0xff << 13)) |
							  (1 << 31);
						s->filter[0].can_id = tmp;

						tmp = ((s->code_mask[4] << 21) & (0xff << 21)) |
							  ((s->code_mask[5] << 13) & (0xff << 13)) |
							  (0xff << 5) | (0xff >> 3) |
							  (7 << 29);
						s->filter[0].can_mask = ~tmp | (1 << 31);


						tmp = ((s->code_mask[2] << 21) & (0xff << 21)) |
							  ((s->code_mask[3] << 13) & (0xff << 13)) |
							  (1 << 31);
						s->filter[1].can_id = tmp;

						tmp = ((s->code_mask[6] << 21) & (0xff << 21)) |
							  ((s->code_mask[7] << 13) & (0xff << 13)) |
							  (0xff << 5) | (0xff >> 3) |
							  (7 << 29);
						s->filter[1].can_mask = ~tmp | (1 << 31);

						// For SFF						
						tmp = ((s->code_mask[0] <<  3) & (0xff <<  3)) |
							  ((s->code_mask[1] >>  5) & 0x07);
						s->filter[2].can_id = tmp;

						tmp = ((s->code_mask[4] <<  3) & (0xff <<  3)) |
							  ((s->code_mask[5] >>  5) & 0x07) |
							  (0xff << 11) | (0xff << 19) | (0x0f << 27);
						s->filter[2].can_mask = ~tmp | (1 << 31);

						if(s->code_mask[1] & (1 << 4)) // RTR
							s->filter[2].can_id |= (1 << 30);
						if(!(s->code_mask[5] & (1 << 4)))
							s->filter[2].can_mask |= (1 << 30);
					
						tmp = ((s->code_mask[2] <<  3) & (0xff <<  3)) |
							  ((s->code_mask[3] >>  5) & 0x07);
						s->filter[3].can_id = tmp;

						tmp = ((s->code_mask[6] <<  3) & (0xff <<  3)) |
							  ((s->code_mask[7] >>  5) & 0x07) |
							  (0xff << 11) | (0xff << 19) | (0x0f << 27);
						s->filter[3].can_mask = ~tmp | (1 << 31);

						if(s->code_mask[3] & (1 << 4)) // RTR
							s->filter[3].can_id |= (1 << 30);
						if(!(s->code_mask[7] & (1 << 4)))
							s->filter[3].can_mask |= (1 << 30);

						qemu_chr_fe_ioctl(s->chr, 4, s->filter);
					}

					s->rxmsg_cnt = 0;
					s->rx_cnt = 0;
				}
				break;

			case SJACMR: // Command register.
				if (0x01 & val) { // Send transmission request.
					buff2frameP(s->tx_buff, &can);
#ifdef DEBUG_FILTER
					display_msg(&can);printf("\n");
#endif
					s->statusP &= ~(3 << 2); // Clear transmission complete status,
											// and Transmit Buffer Status.
					// write to the backends.
					qemu_chr_fe_write(s->chr, (uint8_t *)&can, sizeof(struct can_frame));
					s->statusP |= (3 << 2); // Set transmission complete status,
										   // and Transmit Buffer Status.
					s->statusP &= ~(1 << 5); // Clear transmit status.
					s->interruptP |= 0x02;
					if (s->interrupt_en & 0x02)
						qemu_irq_raise(s->irq);
				} else if ( 0x04 & val) { // Release Receive Buffer
					if (s->rxmsg_cnt <= 0)
						break;

					tmp8 = s->rx_buff[s->rxbuf_start]; count = 0;
					if (tmp8 & (1 << 7)) // EFF
						count += 2;
					count += 3;
					if (!(tmp8 & (1 << 6))) // DATA
						count += (tmp8 & 0x0f);
					s->rxbuf_start += count;
					s->rxbuf_start %= SJA_RCV_BUF_LEN;

					s->rx_cnt -= count;
					s->rxmsg_cnt--;
					if(s->rxmsg_cnt == 0) {
						s->statusP &= ~(1 << 0);
						s->interruptP &= ~(1 << 0);
					}
					if((s->interrupt_en & 0x01) && (s->interruptP == 0)) // no other interrupts.
						qemu_irq_lower(s->irq);
				} else if ( 0x08 & val) { // Clear data overrun
					s->statusP &= ~(1 << 1);
					s->interruptP &= ~(1 << 3);
					if((s->interrupt_en & 0x80) && (s->interruptP == 0)) // no other interrupts.
						qemu_irq_lower(s->irq);
				}
				break;
			case SJASR: // Status register
			case SJAIR: // Interrupt register
				break; // Do nothing
			case SJAIER: // Interrupt enable register
				s->interrupt_en = val;
				break;
			case 16: // RX frame information addr16-28.
				s->statusP |= (1 << 5); // Set transmit status.
			case 17:
			case 18:
			case 19:
			case 20:
			case 21:
			case 22:
			case 23:
			case 24:
			case 25:
			case 26:
			case 27:
			case 28:
				if (s->mode & 0x01) { // Reset mode
					if (addr < 24) {
						s->code_mask[addr - 16] = val;
					}
				} else // Operation mode
					s->tx_buff[addr - 16] = val; // Store to TX buffer directly.
				break;
			case SJACDR:
				s->clock = val;
				break;
		}
	} else { // Basic Mode
		switch (addr) {
			case B_SJACTR: // Control register, addr 0
				if((s->control & 0x01) && ((val & 0x01) == 0)) {
					// Go to operation mode from reset mode.
					s->filter[0].can_id = (s->code << 3) & (0xff << 3);
					tmp = (~(s->mask << 3)) & (0xff << 3);
					tmp |= (1 << 31);// Only Basic CAN Frame.
					s->filter[0].can_mask = tmp;
					qemu_chr_fe_ioctl(s->chr, 1, s->filter);

					s->rxmsg_cnt = 0;
					s->rx_cnt = 0;
				} else if(!(s->control & 0x01) && !(val & 0x01)){
					can_software_reset(s);
				}

				s->control = 0x1f & val;
				break;
			case B_SJACMR: // Command register, addr 1
				if (0x01 & val) { // Send transmission request.
					buff2frameB(s->tx_buff, &can);
#ifdef DEBUG_FILTER
					display_msg(&can);printf("\n");
#endif
					s->statusB &= ~(3 << 2); // Clear transmission complete status,
											// and Transmit Buffer Status.
					// write to the backends.
					qemu_chr_fe_write(s->chr, (uint8_t *)&can, sizeof(struct can_frame));
					s->statusB |= (3 << 2); // Set transmission complete status,
										   // and Transmit Buffer Status.
					s->statusB &= ~(1 << 5); // Clear transmit status.
					s->interruptB |= 0x02;
					if (s->control & 0x04){
						qemu_irq_raise(s->irq);
					}
				} else if ( 0x04 & val) { // Release Receive Buffer
					if (s->rxmsg_cnt <= 0)
						break;

					qemu_mutex_lock(&s->rx_lock);
					tmp8 = s->rx_buff[(s->rxbuf_start + 1) % SJA_RCV_BUF_LEN];
					count = 2 + (tmp8 & 0x0f);
#ifdef DEBUG_FILTER
					printf("\nRelease");
					for(i = 0; i < count; i++) 
						printf(" %02X", s->rx_buff[(s->rxbuf_start + i) % SJA_RCV_BUF_LEN]);
					for(; i < 11; i++) 
						printf("   ");
					printf("==== cnt=%d, count=%d\n", s->rx_cnt, count);
#endif
					s->rxbuf_start += count;
					s->rxbuf_start %= SJA_RCV_BUF_LEN;
					s->rx_cnt -= count;
					s->rxmsg_cnt--;
					qemu_mutex_unlock(&s->rx_lock);

					if(s->rxmsg_cnt == 0) {
						s->statusB &= ~(1 << 0);
						s->interruptB &= ~(1 << 0);
					}
					if((s->control & 0x02) && (s->interruptB == 0)) // no other interrupts.
						qemu_irq_lower(s->irq);
				} else if ( 0x08 & val) { // Clear data overrun
					s->statusB &= ~(1 << 1);
					s->interruptB &= ~(1 << 3);
					if((s->control & 0x10) && (s->interruptB == 0)) // no other interrupts.
						qemu_irq_lower(s->irq);
				}
				break;
			case 4:
				s->code = val;
				break;
			case 5:
				s->mask = val;
				break;
			case 10:
				s->statusB |= (1 << 5); // Set transmit status.
			case 11:
			case 12:
			case 13:
			case 14:
			case 15:
			case 16:
			case 17:
			case 18:
			case 19:
				if ((s->control & 0x01) == 0) { // Operation mode
					s->tx_buff[addr - 10] = val; // Store to TX buffer directly.
				}
				break;
			case SJACDR:
				s->clock = val;
				break;
		}
	}
}  

static uint64_t can_mem_read(void *opaque, hwaddr addr, unsigned size) 
{  
    CanState 			*s = opaque;
	int     			region_size;
	uint64_t 			temp = 0;

	region_size  = memory_region_size(&s->memio);
	DPRINTF("read addr %d, region size %d\n", (int)addr, region_size);
	if(addr > region_size)  
		return 0;

	if (s->clock & 0x80) { // PeliCAN Mode
		switch (addr) {
			case SJAMOD: // Mode register, addr 0
				temp = s->mode;
				break;
			case SJACMR: // Command register, addr 1
				temp = 0x00; // Command register, cannot be read.
				break;
			case SJASR: // Status register, addr 2
				temp = s->statusP;
				break;
			case SJAIR: // Interrupt register, addr 3
				temp = s->interruptP;
				s->interruptP = 0;
				if (s->rxmsg_cnt) {
					s->interruptP |= (1 << 0); // Receive interrupt.
					break;
				}
				qemu_irq_lower(s->irq);
				break;
			case SJAIER: // Interrupt enable register, addr 4
				temp = s->interrupt_en;
				break;
			case 5: // Reserved
			case 6: // Bus timing 0, hardware related, not support now.
			case 7: // Bus timing 1, hardware related, not support now.
			case 8: // Output control register, hardware related, not support now.
			case 9: // Test.
			case 10: // Reserved
			case 11:
			case 12:
			case 13:
			case 14:
			case 15:		
				temp = 0x00; 
				break;

			case 16:
			case 17:
			case 18:
			case 19:
			case 20:
			case 21:
			case 22:
			case 23:
			case 24:
			case 25:
			case 26:
			case 27:
			case 28:
				if (s->mode & 0x01) { // Reset mode
					if (addr < 24)
						temp = s->code_mask[addr - 16];
					else
						temp = 0x00;
				} else { // Operation mode
					temp = s->rx_buff[(s->rxbuf_start + addr - 16) % SJA_RCV_BUF_LEN];
				}
				break;
			case SJACDR:
				temp = s->clock;
				break;
			default:
				temp = 0xff;
		}
	} else { // Basic Mode
		switch (addr) {
			case B_SJACTR: // Control register, addr 0
				temp = s->control;
				break;
			case B_SJASR: // Status register, addr 2
				temp = s->statusB;
				break;
			case B_SJAIR: // Interrupt register, addr 3
				temp = s->interruptB;
				s->interruptB = 0;
				if (s->rxmsg_cnt) {
					s->interruptB |= (1 << 0); // Receive interrupt.
					break;
				}
				qemu_irq_lower(s->irq);
				break;
			case 4:
				temp = s->code;
				break;
			case 5:
				temp = s->mask;
				break;
			case 20:
#ifdef DEBUG_FILTER
				printf("Read   ");
#endif
			case 21:
			case 22:
			case 23:
			case 24:
			case 25:
			case 26:
			case 27:
			case 28:
			case 29:
				temp = s->rx_buff[(s->rxbuf_start + addr - 20) % SJA_RCV_BUF_LEN];
#ifdef DEBUG_FILTER
				printf(" %02X", (unsigned int)(temp & 0xff));
#endif
				break;
			case 31:
				temp = s->clock;
				break;
			default:
				temp = 0xff;
				break;			
		}
	}
	DPRINTF("     %d bytes of 0x%lx from addr %d\n", size, (long unsigned int)temp, (int)addr);

	return temp;
}

static const MemoryRegionOps can_mem_ops = {  
	.read 		= can_mem_read,  
	.write 		= can_mem_write,  
	.endianness = DEVICE_LITTLE_ENDIAN,
    .impl 		= {
		// how many bytes can we read/write every time.
        .min_access_size = 1, 
        .max_access_size = 1,
    },
};


static int canpci_can_receive(void *opaque)
{
    CanState *s = opaque;

	if (s->clock & 0x80) { // PeliCAN Mode
		if(s->mode & 0x01) // reset mode.
			return 0;
	} else { // BasicCAN mode
		if(s->control & 0x01)
			return 0;
	}

	return 1; // always return 1, when operation mode
}


static void canpci_receive(void *opaque, const uint8_t *buf, int size)
{
    CanState *s = opaque;
	static uint8_t rcv[SJA_MSG_MAX_LEN]; 
	int ret, i;

#ifdef DEBUG_FILTER
	printf("#################################################\n");
	display_msg((struct can_frame *)buf);
#endif

	if(size < sizeof(struct can_frame))
		return;

	qemu_mutex_lock(&s->rx_lock); // Just do it quickly :)
	if (s->clock & 0x80) { // PeliCAN Mode
		s->statusP |= (1 << 4); // the CAN controller is receiving a message

		if(accept_filter(s, (struct can_frame*)buf) == 0) {
			s->statusP &= ~(1 << 4);
#ifdef DEBUG_FILTER
			printf("     NOT\n");
#endif
			goto fail;
		}

		if((ret = frame2buffP((struct can_frame*)buf, rcv)) < 0) {
			s->statusP &= ~(1 << 4);
#ifdef DEBUG_FILTER
			printf("     ERR\n");
#endif
			goto fail; // maybe not support now.
		}

		if(s->rx_cnt + ret > SJA_RCV_BUF_LEN) { // Data overrun.
			s->statusP |= (1 << 1); // Overrun status
			s->interruptP |= (1 << 3);
			if (s->interrupt_en & (1 << 3)) // Overrun interrupt enable
				qemu_irq_raise(s->irq);
			s->statusP &= ~(1 << 4);
#ifdef DEBUG_FILTER
			printf("     OVER\n");
#endif
			goto fail;
		}
		s->rx_cnt += ret;
		s->rxmsg_cnt++;
#ifdef DEBUG_FILTER
		printf("     OK\n");
#endif

		for(i = 0; i < ret; i++) {
			s->rx_buff[(s->rx_ptr++) % SJA_RCV_BUF_LEN] = rcv[i];
		}
		s->rx_ptr %= SJA_RCV_BUF_LEN; // update the pointer.

		s->statusP |= 0x01; // Set the Receive Buffer Status. DS-p23
		s->interruptP |= 0x01;
		s->statusP &= ~(1 << 4);
		s->statusP |= (1 << 0);
		if(s->interrupt_en & 0x01) // Receive Interrupt enable.
			qemu_irq_raise(s->irq);
	} else { // BasicCAN mode
		s->statusB |= (1 << 4); // the CAN controller is receiving a message

		if((ret = frame2buffB((struct can_frame*)buf, rcv)) < 0) {
			s->statusB &= ~(1 << 4);
#ifdef DEBUG_FILTER
			printf("     NOT\n");
#endif
			goto fail; // maybe not support now.
		}

		if(s->rx_cnt + ret > SJA_RCV_BUF_LEN) { // Data overrun.
			s->statusB |= (1 << 1); // Overrun status
			s->statusB &= ~(1 << 4);
			s->interruptB |= (1 << 3);
			if (s->control & (1 << 4)) // Overrun interrupt enable
				qemu_irq_raise(s->irq);
#ifdef DEBUG_FILTER
			printf("     OVER\n");
#endif
			goto fail;
		}
		s->rx_cnt += ret;
		s->rxmsg_cnt++;
#ifdef DEBUG_FILTER
		printf("     OK\n");
		printf("RCV B ret=%2d, ptr=%2d cnt=%2d msg=%2d\n", ret, s->rx_ptr, s->rx_cnt, s->rxmsg_cnt); 
#endif
		for(i = 0; i < ret; i++) {
			s->rx_buff[(s->rx_ptr++) % SJA_RCV_BUF_LEN] = rcv[i];
		}
		s->rx_ptr %= SJA_RCV_BUF_LEN; // update the pointer.

		s->statusB |= 0x01; // Set the Receive Buffer Status. DS-p15
		s->statusB &= ~(1 << 4);
		s->interruptB |= 0x01;
		if(s->control & 0x02) { // Receive Interrupt enable.
			qemu_irq_raise(s->irq);
		}
	}
fail:
	qemu_mutex_unlock(&s->rx_lock);
}


static int can_pci_init(PCIDevice *dev)
{
	// Get the address of PCICanState through PCIDevice.
    PCICanState *pci = DO_UPCAST(PCICanState, dev, dev); 
    CanState *s = &pci->state;

	if (!s->chr) {
        fprintf(stderr, "Can't create can device, empty char device\n");
		exit(1);
    }

	if (pci->model) {
		if (strncmp(pci->model, "SJA1000", 256)) { /* for security reason */
		    fprintf(stderr, "Can't create can device, the model %s doesn't support now.\n", pci->model);
			exit(1);
		}
	}

    qemu_register_reset(can_hardware_reset, s);
    qemu_chr_add_handlers(s->chr, canpci_can_receive, canpci_receive, NULL, s);

    pci->dev.config[PCI_INTERRUPT_PIN] = 0x01;
    s->irq = pci->dev.irq[0];

	qemu_irq_lower(s->irq);

    memory_region_init_io(&s->memio, &can_mem_ops, s, "can", PCI_MEM_SIZE);
    pci_register_bar(&pci->dev, MEM_BAR, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->memio);

	can_hardware_reset(s);

    return 0;
}

static void can_pci_exit(PCIDevice *dev)
{
    PCICanState *pci = DO_UPCAST(PCICanState, dev, dev);
    CanState 	  *s = &pci->state;

    qemu_chr_add_handlers(s->chr, NULL, NULL, NULL, NULL);
    qemu_unregister_reset(can_hardware_reset, s);

    memory_region_destroy(&s->memio);
}


static const VMStateDescription vmstate_pci_can = {
    .name 				= "pci-can",
    .version_id 		= PCI_REVISION_ID_CANBUS,
    .minimum_version_id = 1,
    .fields      		= (VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, PCICanState),
        VMSTATE_END_OF_LIST()
    }
};

static Property can_pci_properties[] = {
    DEFINE_PROP_CHR("chardev",  PCICanState, state.chr),
	DEFINE_PROP_STRING("model",  PCICanState, model),
    DEFINE_PROP_END_OF_LIST(),
};

static void can_pci_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass 	*dc = DEVICE_CLASS(klass);
    PCIDeviceClass 	*pc = PCI_DEVICE_CLASS(klass);
	
    pc->init 		= can_pci_init;
    pc->exit 		= can_pci_exit;
    pc->vendor_id 	= PCI_VENDOR_ID_REDHAT; // 0x1b36
    pc->device_id 	= PCI_DEVICE_ID_CANBUS;
    pc->revision 	= PCI_REVISION_ID_CANBUS;
    pc->class_id 	= PCI_CLASS_OTHERS;

    dc->desc 		= "PCI CAN SJA1000";
    dc->vmsd 		= &vmstate_pci_can;
    dc->props 		= can_pci_properties;
}

static const TypeInfo can_pci_info = {
    .name			= "pci-can",
    .parent			= TYPE_PCI_DEVICE,
    .instance_size	= sizeof(PCICanState),
    .class_init		= can_pci_class_initfn,
};

static void can_pci_register_types(void)
{
    type_register_static(&can_pci_info);
}

type_init(can_pci_register_types)

#endif /* __linux__ */

