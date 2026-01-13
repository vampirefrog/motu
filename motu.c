// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   MOTU midi express 128 driver
 *   Code based on the Behringer BCD2000 driver
 *
 *   Copyright (C) 2014 vampirefrog (motu-usb@vampi.tech)
 *   touched 2022 lost-bit (lost-bit@tripod-systems.de)
 *     support for micro express & micro lite added
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/bitmap.h>
#include <linux/usb.h>
#include <linux/usb/audio.h>
#include <linux/version.h>
#include <linux/spinlock.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>

#define PREFIX   "snd-motu: "
#define BUFSIZE  128
#define NUM_ISO  4

typedef enum
{
        express_128,micro_express,micro_lite,express_xt
} en_motu_devices;

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE(0x07fd, 0x0001) },
	{ },
};

struct motu_in_port {
	struct snd_rawmidi_substream *substream;
	unsigned char last_cmd;
	unsigned char cmd_bytes_remaining;
	unsigned char buf[64];
	unsigned int buf_len;
	unsigned int buf_send_len; // how much of the buffer can be sent
};

struct motu_out_port {
	struct snd_rawmidi_substream *substream;
};

#define N_MBUF       64
struct motufifo 
{
    unsigned char  mbuf[N_MBUF];
    unsigned int   p_in,p_out;
    unsigned char  last_cmd;
    unsigned int   rd_bytes;
    unsigned int   missing_bytes;
    unsigned int   buf_len;
    unsigned int   buf_send_len;
    unsigned int   cmd_len;
    unsigned int   remaining;
};

struct motu {
	struct usb_device *dev;
	struct snd_card *card;
	struct usb_interface *intf;
	int card_index;

	int midi_out_active;
	struct snd_rawmidi *rmidi;
	struct motu_in_port in_ports[9];
	struct motu_out_port out_ports[9];
	unsigned char counter;

	unsigned char midi_in_buf[BUFSIZE];
	unsigned char midi_out_buf[BUFSIZE];

	struct urb *midi_out_urb;
	struct urb *midi_in_urb;

	struct usb_anchor anchor;
	
	en_motu_devices motu_type;
	int n_ports_in;
	int n_ports_out;
	int last_out_port;
	int last_in_port;
	int in_state;
	
	struct motufifo mfifo[9];
	
	spinlock_t spinlock;
};

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;

static DEFINE_MUTEX(devices_mutex);
static DECLARE_BITMAP(devices_used, SNDRV_CARDS);
static struct usb_driver motu_driver;

#ifdef CONFIG_SND_DEBUG
static void motu_dump_buffer(const char *prefix, const char *buf, int len) {
	print_hex_dump(KERN_DEBUG, prefix,
			DUMP_PREFIX_NONE, 16, 1,
			buf, len, false);
}
#else
static void motu_dump_buffer(const char *prefix, const char *buf, int len) {}
#endif


static int motu_midi_input_open(struct snd_rawmidi_substream *substream)
{
	return 0;
}

static int motu_midi_input_close(struct snd_rawmidi_substream *substream)
{
	return 0;
}

/* (de)register midi substream from client */
static void motu_midi_input_trigger(struct snd_rawmidi_substream *substream, int up) {
	struct motu *motu = substream->rmidi->private_data;

	if(substream->number >= motu->n_ports_in)
		return;

	motu->in_ports[substream->number].substream = up ? substream : NULL;
}

static int get_cmd_num_bytes(unsigned char b) {
	static const int num_bytes[] = {
		/* 8x */ 3,
		/* 9x */ 3,
		/* Ax */ 3,
		/* Bx */ 3,
		/* Cx */ 2,
		/* Dx */ 2,
		/* Ex */ 3,
	};

	static const int fx_bytes[] = {
		/* F0 */ -1,
		/* F1 */  2,
		/* F2 */  1,
		/* F3 */  2,
		/* F4 */ -1,
		/* F5 */ -1,
		/* F6 */  1,
		/* F7 */  1,
		/* F8 */  1,
		/* F9 */  1,
		/* FA */  1,
		/* FB */  1,
		/* FC */  1,
		/* FD */ -1,
		/* FE */  1,
		/* FF */  1,
	};

	if(b >= 0xf0)
		return fx_bytes[b & 0x0f];

	if(b >= 0x80)
		return num_bytes[(b >> 4) - 8];

	return -1;
}

static void motu_in_port_append_byte(struct motu *motu, int port, unsigned char b) {
	if(motu->in_ports[port].buf_len < sizeof(motu->in_ports[port].buf))
		motu->in_ports[port].buf[motu->in_ports[port].buf_len++] = b;
}

static void motu_in_port_write_byte(struct motu *motu, int port, unsigned char b) {
	int num_bytes;
	struct motu_in_port *in_port;

	in_port = &motu->in_ports[port];
	num_bytes = get_cmd_num_bytes(b) - 1;
	if(num_bytes == 0) {
		in_port->last_cmd = 0;
		in_port->cmd_bytes_remaining = 0;
		motu_in_port_append_byte(motu, port, b);
		in_port->buf_send_len = in_port->buf_len;
	} else if(num_bytes > 0) {
		in_port->last_cmd = b;
		in_port->cmd_bytes_remaining = num_bytes;
		in_port->buf_send_len = in_port->buf_len;
		motu_in_port_append_byte(motu, port, b);
	} else if(in_port->last_cmd > 0) {
		if(in_port->cmd_bytes_remaining <= 0) {
			in_port->buf_send_len = in_port->buf_len;
			motu_in_port_append_byte(motu, port, in_port->last_cmd);
			num_bytes = get_cmd_num_bytes(in_port->last_cmd) - 1;
			in_port->cmd_bytes_remaining = num_bytes;
		}
		in_port->cmd_bytes_remaining--;
		motu_in_port_append_byte(motu, port, b);
		if(in_port->cmd_bytes_remaining == 0) {
			in_port->buf_send_len = in_port->buf_len;
		}
	} else {
		// in a normal stream, this shouldn't be reached
		motu_in_port_append_byte(motu, port, b);
		in_port->buf_send_len = in_port->buf_len;
	}
}

static int motu_in_port_get_buf_size(struct motu *motu, int port) {
	if(motu->in_ports[port].buf_len <= motu->in_ports[port].buf_send_len)
		return motu->in_ports[port].buf_len;

	return motu->in_ports[port].buf_send_len;
}

static void motu_in_port_flush(struct motu *motu, int port) {
	if(motu->in_ports[port].buf_send_len == 0)
		return;

	// Example:
	// Note ON: 90 40 7f (Channel 1, note 40, velocity 127)

	if(motu->in_ports[port].buf_len > motu->in_ports[port].buf_send_len) {
		memcpy(motu->in_ports[port].buf, motu->in_ports[port].buf + motu->in_ports[port].buf_send_len, motu->in_ports[port].buf_len - motu->in_ports[port].buf_send_len);
	}
	motu->in_ports[port].buf_len -= motu->in_ports[port].buf_send_len;
	motu->in_ports[port].buf_send_len = 0;
}

static void motu_midi_handle_input_prot1(struct motu *motu, const unsigned char *buf, unsigned int buf_len) {
	struct snd_rawmidi_substream *midi_receive_substream;

	int i, p;

	// parsing state machine
	int in_data = 0;
	uint8_t mask = 0;
	uint8_t chan = 0;

	motu_dump_buffer(PREFIX "received from device: ", buf, buf_len);

	if (buf_len < 2)
		return;

	for(i = 2; i < buf_len; i++) {
		if(in_data) {
			for(; chan < 8 && mask != 0; chan++) {
				if((mask & 1) != 0) {
					motu_in_port_write_byte(motu, chan, buf[i]);
					mask >>= 1;
					chan++;
					break;
				}
				mask >>= 1;
			}
			if(mask == 0) {
				in_data = 0;
			}
		} else {
			in_data = 1;
			chan = 0;
			mask = buf[i];
			if(mask == 0) {
				in_data = 0;
			}
		}
	}

	for(p = 0; p < 8; p++) {
		int len = motu_in_port_get_buf_size(motu, p);
		if(len > 0) {
			motu_dump_buffer(PREFIX "sending to userspace: ", motu->in_ports[p].buf, len);
			if(motu->in_ports[p].substream) {
				midi_receive_substream = READ_ONCE(motu->in_ports[p].substream);
				if(midi_receive_substream != 0) {
					snd_rawmidi_receive(midi_receive_substream, motu->in_ports[p].buf, len);
				}
			}
			motu_in_port_flush(motu, p);
		}
	}
}

static void motu_midi_handle_input_prot2(struct motu *motu, const unsigned char *buf, unsigned int buf_len) {
   struct snd_rawmidi_substream *midi_receive_substream;

     int  i;

     // ignore 1st byte
     i = 1;

     while(i < buf_len)
     {
        switch(motu->in_state)
        {
           case 0 :
              if(buf[i] == 0xF5)
                 motu->in_state = 1;
              break;
           case 1 : // desired port
              if(buf[i] != 0xFF)
              {
                 // Validate port number to prevent buffer overflow
                 if(buf[i] >= motu->n_ports_in) {
                    dev_warn(&motu->dev->dev, PREFIX
                       "invalid port number %d (max %d), resetting input state\n",
                       buf[i], motu->n_ports_in - 1);
                    motu->in_state = 0;
                    break;
                 }
                 motu->last_in_port = buf[i];
                 motu->in_ports[motu->last_in_port].buf_len = 0;
                 motu->in_state = 2;
              }
              break;
           case 2 : // data section
              if(buf[i] != 0xFF)
              {
                 if((buf[i] & 0x80) == 0)
                 {
                    // Check buffer space before writing
                    if(motu->in_ports[motu->last_in_port].buf_len >= sizeof(motu->in_ports[motu->last_in_port].buf)) {
                       dev_warn(&motu->dev->dev, PREFIX "input buffer overflow on port %d, dropping data\n", motu->last_in_port);
                       motu->in_state = 0;
                       break;
                    }
                    motu->in_ports[motu->last_in_port].buf[motu->in_ports[motu->last_in_port].buf_len++] =
                       motu->in_ports[motu->last_in_port].last_cmd;
                 }
                 else
                 {
                    motu->in_ports[motu->last_in_port].last_cmd = buf[i];
                 }
                 // Check buffer space before writing
                 if(motu->in_ports[motu->last_in_port].buf_len >= sizeof(motu->in_ports[motu->last_in_port].buf)) {
                    dev_warn(&motu->dev->dev, PREFIX "input buffer overflow on port %d, dropping data\n", motu->last_in_port);
                    motu->in_state = 0;
                    break;
                 }
                 motu->in_ports[motu->last_in_port].buf[motu->in_ports[motu->last_in_port].buf_len++] = buf[i];
                 switch(motu->in_ports[motu->last_in_port].last_cmd)
                 {
                    case 0xF5 : 
                       motu->in_state = 1;
                       break;
                    case 0xF0 :
                       motu->in_state = 4; // special command
                       break;
                    default :
                       if(buf[i] < 0xF0)
                          motu->in_ports[motu->last_in_port].cmd_bytes_remaining = get_cmd_num_bytes(motu->in_ports[motu->last_in_port].last_cmd);
                       else
                          motu->in_ports[motu->last_in_port].cmd_bytes_remaining = 3;
                       motu->in_state = 3;
                       break;
                 }
              }
              break;
           case 3 :
           case 4 :
              if(buf[i] != 0xFF)
              {
                 // Check buffer space before writing
                 if(motu->in_ports[motu->last_in_port].buf_len >= sizeof(motu->in_ports[motu->last_in_port].buf)) {
                    dev_warn(&motu->dev->dev, PREFIX "input buffer overflow on port %d, dropping data\n", motu->last_in_port);
                    motu->in_state = 0;
                    break;
                 }
                 motu->in_ports[motu->last_in_port].buf[motu->in_ports[motu->last_in_port].buf_len++] = buf[i];
                 if(((motu->in_state == 3) && (motu->in_ports[motu->last_in_port].buf_len == motu->in_ports[motu->last_in_port].cmd_bytes_remaining)) ||
                    ((motu->in_state == 4) && (buf[i] == 0xF7)))
                 {
                    if(motu->in_ports[motu->last_in_port].substream)
                    {
                       midi_receive_substream = READ_ONCE(motu->in_ports[motu->last_in_port].substream);
                       if(midi_receive_substream != 0)
                       {
                          snd_rawmidi_receive(midi_receive_substream,motu->in_ports[motu->last_in_port].buf,motu->in_ports[motu->last_in_port].buf_len);
                       }
                    }
                    motu->in_ports[motu->last_in_port].buf_len = 0;
                    motu->in_state = 2;
                 }
              }
              break;
        } /* switch */
        i++;
     }
        
} /* motu_midi_handle_input_prot2 */

static void motu_midi_send_prot1(struct motu *motu)
{
	int ret, p, i, mask, bit;
	struct snd_rawmidi_substream *midi_out_substream;
	int lens[8];
	unsigned char bufs[8][3];
	int outlen = 2;

	motu->midi_out_buf[0] = motu->counter++;
	motu->midi_out_buf[1] = 0;

	for(p = 0; p < motu->n_ports_out; p++) {
		lens[p] = 0;

		midi_out_substream = READ_ONCE(motu->out_ports[p].substream);
		if (!midi_out_substream)
			continue;

		lens[p] = snd_rawmidi_transmit(midi_out_substream, bufs[p], 3);

		if (lens[p] < 0)
			dev_err(&motu->dev->dev, "%s: snd_rawmidi_transmit error %d\n", __func__, lens[p]);
	}

	for(i = 0; i < 3; i++) {
		mask = 0;
		for(p = 0, bit = 1; p < motu->n_ports_out; p++, bit <<= 1) {
			if(lens[p] > i) mask |= bit;
		}
		if(mask == 0) {
			motu->midi_out_active = 0;
			break;
		}
		if(outlen < sizeof(motu->midi_out_buf) / sizeof(motu->midi_out_buf[0]))
			motu->midi_out_buf[outlen++] = mask;
		for(p = 0; p < motu->n_ports_out; p++) {
			if(lens[p] > i && outlen < sizeof(motu->midi_out_buf) / sizeof(motu->midi_out_buf[0]))
				motu->midi_out_buf[outlen++] = bufs[p][i];
		}
	}

	if(outlen <= 2)
		return;
	if(outlen < sizeof(motu->midi_out_buf) / sizeof(motu->midi_out_buf[0]))
		motu->midi_out_buf[outlen++] = 0;
	if(outlen < sizeof(motu->midi_out_buf) / sizeof(motu->midi_out_buf[0]))
		motu->midi_out_buf[outlen++] = 0;

	/* set payload length */
	motu->midi_out_urb->transfer_buffer_length = outlen;

	motu_dump_buffer(PREFIX "sending to device: ", motu->midi_out_buf, outlen);

	/* send packet to the MOTU */
	ret = usb_submit_urb(motu->midi_out_urb, GFP_ATOMIC);
	if (ret < 0)
		dev_err(&motu->dev->dev, PREFIX
			"%s (%p): usb_submit_urb() failed, ret=%d, outlen=%d\n",
			__func__, midi_out_substream, ret, outlen);
	else
		motu->midi_out_active = 1;
}

static void mfifo_in(struct motu *motu,int port,unsigned char *buf,int len)
{
        int  i;

        for(i=0; i<len; i++)
        {
           // Check if FIFO is full
           if(motu->mfifo[port].buf_len >= N_MBUF) {
              dev_warn(&motu->dev->dev, PREFIX "FIFO overflow on port %d, dropping data\n", port);
              return;
           }

           motu->mfifo[port].mbuf[motu->mfifo[port].p_in] = buf[i];
           motu->mfifo[port].p_in++;
           if(motu->mfifo[port].p_in >= N_MBUF)
              motu->mfifo[port].p_in = 0;

           motu->mfifo[port].buf_len++;
           if(buf[i] & 0x80) // command
           {
              switch(buf[i])
              {
                 case 0xF0 : // sysex command
                    motu->mfifo[port].cmd_len = 0;
                    break;
                 case 0xF7 : // end sysex
                    motu->mfifo[port].cmd_len = 0;
                    motu->mfifo[port].buf_send_len = motu->mfifo[port].buf_len;
                    break;
                 default :
                    motu->mfifo[port].cmd_len = get_cmd_num_bytes(buf[i]) - 1;
                    motu->mfifo[port].remaining = motu->mfifo[port].cmd_len;
                    break;
	      }
           }
           else if(motu->mfifo[port].cmd_len)
           {
              if(motu->mfifo[port].remaining == 0)
                 motu->mfifo[port].remaining = motu->mfifo[port].cmd_len;
              motu->mfifo[port].remaining--;
              if(motu->mfifo[port].remaining == 0)
              {
                 motu->mfifo[port].buf_send_len = motu->mfifo[port].buf_len;
              }
           }
        }

        return;
        
} /* mfifo_in */

static void motu_midi_send_prot2(struct motu *motu)
{
	int ret, p, i,j,k;
	struct snd_rawmidi_substream *midi_out_substream;
	int lens[9];
	unsigned char bufs[9][12];
	uint8_t state;
	int out_count, out_offset;

	for(p = 0; p < motu->n_ports_out; p++) {
		lens[p] = 0;

		midi_out_substream = READ_ONCE(motu->out_ports[p].substream);
		if (!midi_out_substream)
			continue;

		lens[p] = snd_rawmidi_transmit(midi_out_substream, bufs[p], 3);
		mfifo_in(motu,p,bufs[p],lens[p]);

		if (lens[p] < 0)
			dev_err(&motu->dev->dev, "%s: snd_rawmidi_transmit error %d\n", __func__, lens[p]);
	}

        i = 0;
        state = 0;
        k = 0;
        for(p=0; p<motu->n_ports_out; p++)
        {
           while(motu->mfifo[p].buf_send_len)
           {
              if(p != motu->last_out_port)
              {
                 if(k < 10) // dont split channel-change
                 {
                    if(i + 3 >= BUFSIZE) {
                       dev_warn(&motu->dev->dev, PREFIX "output buffer full, stopping\n");
                       goto send_buffer;
                    }
                    motu->midi_out_buf[i++] = 0xF5;
                    motu->midi_out_buf[i++] = p;
                    k += 2;
                    motu->last_out_port = p;
                    if((motu->mfifo[p].mbuf[motu->mfifo[p].p_out] & 0x80) == 0)
                    {
                       motu->midi_out_buf[i++] = motu->mfifo[p].last_cmd;
                       k++;
                    }
                 }
                 else
                 {
                    while(k < 12)
                    {
                       if(i >= BUFSIZE) {
                          dev_warn(&motu->dev->dev, PREFIX "output buffer full, stopping\n");
                          goto send_buffer;
                       }
                       motu->midi_out_buf[i++] = 0xFF;
                       k++;
                    }
                 }
              }
              else
              {
                 if(i >= BUFSIZE) {
                    dev_warn(&motu->dev->dev, PREFIX "output buffer full, stopping\n");
                    goto send_buffer;
                 }
                 if(motu->mfifo[p].mbuf[motu->mfifo[p].p_out] & 0x80)
                    motu->mfifo[p].last_cmd = motu->mfifo[p].mbuf[motu->mfifo[p].p_out];
                 motu->midi_out_buf[i++] =
                     motu->mfifo[p].mbuf[motu->mfifo[p].p_out];
                 motu->mfifo[p].p_out++;
                 if(motu->mfifo[p].p_out >= N_MBUF)
                    motu->mfifo[p].p_out = 0;
                 motu->mfifo[p].buf_len--;
                 motu->mfifo[p].buf_send_len--;
                 k++;
              }
              if(k == 12)
              {
                 if(i + 2 >= BUFSIZE) {
                    dev_warn(&motu->dev->dev, PREFIX "output buffer full, stopping\n");
                    goto send_buffer;
                 }
                 motu->midi_out_buf[i++] = 1;
                 motu->midi_out_buf[i++] = 0;
                 k = 0;
              }
           }
        }

send_buffer:
        j = 0;
        if(i)
        {
           if(k)
           {
              // fill rest
              while(k < 12 && i < BUFSIZE)
              {
                 motu->midi_out_buf[i++] = 0xFF;
                 k++;
	      }
              if(i + 2 <= BUFSIZE) {
                 motu->midi_out_buf[i++] = 1;
                 motu->midi_out_buf[i++] = 0;
              }
           }
                         
           out_count = i;
           out_offset = 0;
           k = 0;
           while(out_count > 0)
           {
              if(out_count > 14)
                 j = 14;
	      else
	         j = out_count;
              if(k < NUM_ISO)
              {
                 motu->midi_out_urb->iso_frame_desc[k].offset = out_offset;
	         motu->midi_out_urb->iso_frame_desc[k].length = j;
                 motu->midi_out_urb->iso_frame_desc[k].status = 0;
                 k++;
              }
              out_offset += j;
              out_count -= j;                 
           }
           motu->midi_out_urb->number_of_packets = k;
           
	   motu_dump_buffer(PREFIX "sending to device    : ", motu->midi_out_buf, i);

           /* send packet to the MOTU */
	   ret = usb_submit_urb(motu->midi_out_urb, GFP_ATOMIC);
	   if (ret < 0)
		dev_err(&motu->dev->dev, PREFIX
			"%s (%p): usb_submit_urb() failed, ret=%d, outlen=%d\n",
			__func__, midi_out_substream, ret, i);
	   else
		motu->midi_out_active = 1;		
        }

} /* motu_midi_send_prot2 */

static int motu_midi_output_open(struct snd_rawmidi_substream *substream)
{
	return 0;
}

static int motu_midi_output_close(struct snd_rawmidi_substream *substream)
{
	struct motu *motu = substream->rmidi->private_data;

	if (motu->midi_out_active) {
		usb_kill_urb(motu->midi_out_urb);
		motu->midi_out_active = 0;
	}

	return 0;
}

/* (de)register midi substream from client */
static void motu_midi_output_trigger(struct snd_rawmidi_substream *substream,
						int up)
{
	struct motu *motu;
	unsigned long flags;
	
	if(substream == NULL)
	   return;
	if(substream->rmidi == NULL)
	   return;
	if(substream->rmidi->private_data == NULL)
	   return;
	   
	motu = substream->rmidi->private_data;
        spin_lock_irqsave(&motu->spinlock,flags);

	if (up) {
		motu->out_ports[substream->number].substream = substream;
		/* check if there is data userspace wants to send */
		if (!motu->midi_out_active)
		{
		   switch(motu->motu_type)
		   {
		      case express_128 :
		      case micro_lite :
			 motu_midi_send_prot1(motu);
			 break;
		      case micro_express :
		      case express_xt :
		         motu_midi_send_prot2(motu);
		         break;
	           }
		}
	} else {
		motu->out_ports[substream->number].substream = NULL;
	}
	spin_unlock_irqrestore(&motu->spinlock,flags);
}

static void motu_output_complete(struct urb *urb)
{
	struct motu *motu;
	unsigned long flags;

	if (urb->status)
		dev_warn(&urb->dev->dev,
			PREFIX "output urb->status: %d\n", urb->status);

	if (urb->status == -ESHUTDOWN)
		return;

        motu = urb->context;
        if (!motu)
		return;

        spin_lock_irqsave(&motu->spinlock,flags);
	motu->midi_out_active = 0;

	/* check if there is more data userspace wants to send */
	switch(motu->motu_type)
	{
	   case express_128 : 
	   case micro_lite :
	      motu_midi_send_prot1(motu);
	      break;
	   case micro_express :
	   case express_xt :
	      motu_midi_send_prot2(motu);
	      break;
	}

	spin_unlock_irqrestore(&motu->spinlock,flags);
}

static void motu_input_complete(struct urb *urb)
{
	int ret;
	struct motu *motu = urb->context;

	if (urb->status)
		dev_warn(&urb->dev->dev,
			PREFIX "input urb->status: %i\n", urb->status);

	if (!motu || urb->status == -ESHUTDOWN)
		return;

	if (urb->actual_length > 0) {
	    switch(motu->motu_type)
	    {
	       case express_128 :
	       case micro_lite :
  		  motu_midi_handle_input_prot1(motu, urb->transfer_buffer, urb->actual_length);
  		  break;
	       case micro_express :
	       case express_xt :
	          motu_midi_handle_input_prot2(motu, urb->transfer_buffer, urb->actual_length);
	          break;
            }
	}

	/* return URB to device */
	ret = usb_submit_urb(motu->midi_in_urb, GFP_ATOMIC);
	if (ret < 0)
		dev_err(&motu->dev->dev, PREFIX
			"%s: usb_submit_urb() failed, ret=%d\n",
			__func__, ret);
}

static const struct snd_rawmidi_ops motu_midi_output = {
	.open =    motu_midi_output_open,
	.close =   motu_midi_output_close,
	.trigger = motu_midi_output_trigger,
};

static const struct snd_rawmidi_ops motu_midi_input = {
	.open =    motu_midi_input_open,
	.close =   motu_midi_input_close,
	.trigger = motu_midi_input_trigger,
};

static void motu_init_device(struct motu *motu) {
	int ret;

	init_usb_anchor(&motu->anchor);
	usb_anchor_urb(motu->midi_out_urb, &motu->anchor);
	usb_anchor_urb(motu->midi_in_urb, &motu->anchor);

	motu->midi_out_active = 0;

	/* pass URB to device to enable button and controller events */
	ret = usb_submit_urb(motu->midi_in_urb, GFP_KERNEL);
	if (ret < 0)
		dev_err(&motu->dev->dev, PREFIX
			"%s: usb_submit_urb() in failed, ret=%d: ",
			__func__, ret);

	/* ensure initialization is finished */
	usb_wait_anchor_empty_timeout(&motu->anchor, 1000);
}

static int motu_init_midi(struct motu *motu)
{
	int ret;
	struct snd_rawmidi *rmidi;

	ret = snd_rawmidi_new(
		motu->card,
		motu->card->shortname,
		0,
		motu->n_ports_out, /* output */
		motu->n_ports_in, /* input */
		&rmidi
	);

	if (ret < 0)
		return ret;

	strncpy(rmidi->name, motu->card->shortname, sizeof(rmidi->name));

	rmidi->info_flags = SNDRV_RAWMIDI_INFO_DUPLEX;
	rmidi->private_data = motu;

	rmidi->info_flags |= SNDRV_RAWMIDI_INFO_OUTPUT;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &motu_midi_output);

	rmidi->info_flags |= SNDRV_RAWMIDI_INFO_INPUT;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &motu_midi_input);

	motu->rmidi = rmidi;

	usb_set_interface(motu->dev, 1, 2);

	motu->midi_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	switch(motu->motu_type)
	{
	   case express_128 :
	   case micro_lite :
	      motu->midi_out_urb = usb_alloc_urb(0, GFP_KERNEL);
	      break;
           case micro_express :
           case express_xt :
              motu->midi_out_urb = usb_alloc_urb(NUM_ISO, GFP_KERNEL);
              break;
        }

	if (!motu->midi_in_urb || !motu->midi_out_urb) {
		dev_err(&motu->dev->dev, PREFIX "usb_alloc_urb failed\n");
		return -ENOMEM;
	}

	usb_fill_int_urb(motu->midi_in_urb, motu->dev,
				usb_rcvintpipe(motu->dev, 0x81),
				motu->midi_in_buf, BUFSIZE,
				motu_input_complete, motu, 1);

        if((motu->motu_type == express_128) || (motu->motu_type == micro_lite))
        {
	   usb_fill_int_urb(motu->midi_out_urb, motu->dev,
				usb_sndintpipe(motu->dev, 0x02),
				motu->midi_out_buf, BUFSIZE,
				motu_output_complete, motu, 1);
	}
	else if((motu->motu_type == micro_express) || (motu->motu_type == express_xt))
	{
           motu->midi_out_urb->dev = motu->dev;
           motu->midi_out_urb->pipe = usb_sndisocpipe(motu->dev,0x02);
           motu->midi_out_urb->transfer_flags = URB_ISO_ASAP;
           motu->midi_out_urb->transfer_buffer = motu->midi_out_buf;
           motu->midi_out_urb->transfer_buffer_length = BUFSIZE;
           motu->midi_out_urb->complete = motu_output_complete;
           motu->midi_out_urb->context = motu;
           motu->midi_out_urb->start_frame = 0;
           motu->midi_out_urb->number_of_packets = 1;
           motu->midi_out_urb->iso_frame_desc[0].offset = 0;
           motu->midi_out_urb->iso_frame_desc[0].length = BUFSIZE;
           motu->midi_out_urb->interval = 1;
        }
				
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0)
	/* sanity checks of EPs before actually submitting */
	if (usb_urb_ep_type_check(motu->midi_in_urb) ||
	    usb_urb_ep_type_check(motu->midi_out_urb)) {
		dev_err(&motu->dev->dev, "invalid MIDI EP\n");
		return -EINVAL;
	}
#endif	

	motu_init_device(motu);

	return 0;
}

static void motu_free_usb_related_resources(struct motu *motu,
						struct usb_interface *interface)
{
	/* usb_kill_urb not necessary, urb is aborted automatically */

	usb_free_urb(motu->midi_out_urb);
	usb_free_urb(motu->midi_in_urb);

	if (motu->intf) {
		usb_set_intfdata(motu->intf, NULL);
		motu->intf = NULL;
	}
}

static int motu_probe(struct usb_interface *interface,
				const struct usb_device_id *usb_id)
{
	struct snd_card *card;
	struct motu *motu;
	unsigned int card_index;
	char usb_path[32];
	int err, i;
	struct usb_device *usbdev;
	char str[64];

	mutex_lock(&devices_mutex);

	for (card_index = 0; card_index < SNDRV_CARDS; ++card_index)
		if (!test_bit(card_index, devices_used))
			break;

	if (card_index >= SNDRV_CARDS) {
		mutex_unlock(&devices_mutex);
		return -ENOENT;
	}

	usbdev = interface_to_usbdev(interface);

	if(usb_string(usbdev,usbdev->descriptor.iProduct,str,sizeof(str)) <= 0)
        {
           mutex_unlock(&devices_mutex);
	   return -ENODEV;
	}

	if((interface->altsetting->desc.bInterfaceNumber != 1) ||
	   ((usbdev->descriptor.bDeviceSubClass != 3) &&
	    (usbdev->descriptor.bDeviceSubClass != 1))) {
		mutex_unlock(&devices_mutex);
		return -ENOENT;
	}
		
	err = snd_card_new(&interface->dev, index[card_index], id[card_index],
			THIS_MODULE, sizeof(*motu), &card);
	if (err < 0) {
		mutex_unlock(&devices_mutex);
		return err;
	}

	motu = card->private_data;
	motu->dev = usbdev;
	motu->card = card;
	motu->card_index = card_index;
	motu->intf = interface;

        // check iProduct doesn't help: micro lite is 2; micro express also
	switch(usbdev->descriptor.bDeviceSubClass)
	{
	   case 1 : // micro express
	      if(usbdev->actconfig->desc.bConfigurationValue != 1)
	      {
   	         usb_driver_set_configuration(usbdev,1);
   	         return -ENODEV;
	      }
	      usb_set_interface(usbdev,0,0);
	      if(strstr(str,"Micro Express"))
	      {
	         motu->motu_type = micro_express;
	         motu->n_ports_in = 5; // 0 is dead for the moment
	         motu->n_ports_out = 7; // 0 is all
	         motu->last_out_port = -1;
	         motu->last_in_port = -1;
	         motu->in_state = 0;
	      }
	      else
	      {
	         motu->motu_type = express_xt;
	         motu->n_ports_in = 9; // 0 is dead for the moment
	         motu->n_ports_out = 9; // 0 is all
	         motu->last_out_port = -1;
	         motu->last_in_port = -1;
	         motu->in_state = 0;
	      }
	      break;
	   case 3 : // express 128
	      if(strstr(str,"micro lite"))
	      {
	         motu->motu_type = micro_lite;
	         motu->n_ports_in = 5;
	         motu->n_ports_out = 5;
	      }
	      else
	      {
	         motu->motu_type = express_128;
	         motu->n_ports_in = 8;
	         motu->n_ports_out = 8;
	      }
	      break;
	   default :
              mutex_unlock(&devices_mutex);
	      return -ENODEV;
	      break;
	}
	
	spin_lock_init(&motu->spinlock);
	
	// Do I need to initialize this to zero? Or is it already zeroed by snd_card_new()?
	for(i = 0; i < 9; i++) {
		motu->in_ports[i].substream = 0;
		motu->in_ports[i].last_cmd = 0;
		motu->in_ports[i].cmd_bytes_remaining = 0;
		motu->out_ports[i].substream = 0;
	}

	snd_card_set_dev(card, &interface->dev);

	strncpy(card->driver, "snd-motu", sizeof(card->driver));
        snprintf(card->shortname, sizeof(card->shortname), "MOTU %s", str);
        usb_make_path(motu->dev, usb_path, sizeof(usb_path));
        snprintf(motu->card->longname, sizeof(motu->card->longname),
	         "MOTU midi %s at %s",str,usb_path);
			
	err = motu_init_midi(motu);
	if (err < 0)
		goto probe_error;

	err = snd_card_register(card);
	if (err < 0)
		goto probe_error;

	usb_set_intfdata(interface, motu);
	set_bit(card_index, devices_used);

	mutex_unlock(&devices_mutex);
	return 0;

probe_error:
	dev_info(&motu->dev->dev, PREFIX "error during probing");
	motu_free_usb_related_resources(motu, interface);
	snd_card_free(card);
	mutex_unlock(&devices_mutex);
	return err;
}

static void motu_disconnect(struct usb_interface *interface)
{
	struct motu *motu = usb_get_intfdata(interface);

	if (!motu)
		return;

	mutex_lock(&devices_mutex);

	/* make sure that userspace cannot create new requests */
	snd_card_disconnect(motu->card);

	motu_free_usb_related_resources(motu, interface);

	clear_bit(motu->card_index, devices_used);

	snd_card_free_when_closed(motu->card);

	mutex_unlock(&devices_mutex);
}

static int motu_ioctl(struct usb_interface *intf,unsigned int code,void *buf)
{
        return(0);
} /* motu_ioctl */

static struct usb_driver motu_driver = {
	.name =		"snd-motu",
	.probe =	motu_probe,
	.disconnect =	motu_disconnect,
	.unlocked_ioctl = motu_ioctl,
	.id_table =	id_table,
};

module_usb_driver(motu_driver);

MODULE_DEVICE_TABLE(usb, id_table);
MODULE_AUTHOR("vampirefrog, motu-usb@vampi.tech");
MODULE_AUTHOR("lost-bit, lost-bit@tripod-systems.de");
MODULE_DESCRIPTION("MOTU midi express devices driver");
MODULE_LICENSE("GPL");
