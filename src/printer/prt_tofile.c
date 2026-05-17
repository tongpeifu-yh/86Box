/*
 * Print to File support
*/

#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <wchar.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/pit.h>
#include <86box/path.h>
#include <86box/plat.h>
#include <86box/lpt.h>
#include <86box/printer.h>
#include <86box/prt_devs.h>
#include "cpu.h"

#define BUFFER_LEN 4096

typedef struct _printer_t{
    const char *name;

    void *lpt;

    pc_timer_t pulse_timer;
    pc_timer_t timeout_timer;

    // Output file. Initialized with NULL. Once time out (which means a print job is finished),
    // this file will be closed and reset to NULL.
    // Once writing data when it is NULL, open a new file to write.
    FILE *file;
    // FILE *log;

    // buffer and buffer cursor
    char buffer[BUFFER_LEN];
    uint32_t bcursor;

    // Handshake data. All printer devices need these.
    uint8_t data;
    int8_t  ack;
    int8_t  select;
    int8_t  busy;
    int8_t  int_pending;
    int8_t  error;
    int8_t  autofeed;
    uint8_t ctrl;
}printer_t;

static 
void flush_buffer(printer_t *printer)
{
    if(printer == NULL)
        return;
    if(printer->bcursor != 0)// buffer is not empty
    {
        // If no file opened, then open a new one
        if(printer->file == NULL){
            // Use current time as file name and open a file
            char filename[256];
            char fullname[4096];
            time_t now = time(NULL);
            strftime(filename, sizeof(filename), "print_%Y%m%d_%H%M%S.prn", localtime(&now));
            path_append_filename(fullname, usr_path, "printer");
            // if not exists, create the directory
            if (!plat_dir_check(fullname))
                plat_dir_create(fullname);
            path_slash(fullname);
            strcat(fullname, filename);
            printer->file = plat_fopen(fullname, "ab");
            // if fail to create file, return without writing
            if(printer->file == NULL) {
                fprintf(stderr, "Can't open file %s\n", fullname);
                // Here we still need to reset the buffer cursor, otherwise it 
                // will cross the boundary at next writing.
                printer->bcursor = 0;
                return;
            }
        }
        fwrite(printer->buffer, 1, printer->bcursor, printer->file);
        printer->bcursor = 0;
    }
}

// Following are two timer functions. Needed by all printer devices.
static void
pulse_timer(void *priv)
{
    printer_t *dev = (printer_t *) priv;

    if (dev->ack) {
        dev->ack = 0;
        lpt_irq(dev->lpt, 1);
    }

    timer_disable(&dev->pulse_timer);
}

static void
timeout_timer(void *priv)
{
    printer_t *dev = (printer_t *) priv;

    flush_buffer(dev);
    // fprintf(dev->log, "Printer time out\n");
    // A print job is finished. Close the file.
    if(dev->file != NULL){
        fclose(dev->file);
        dev->file = NULL;
    }
    timer_stop(&dev->timeout_timer);
}

static void
reset_printer(printer_t *printer)
{
    if(printer == NULL)
        return;

    flush_buffer(printer);
    if(printer->file != NULL){
        fclose(printer->file);
        printer->file = NULL;
    }

    printer->ack=0;
    printer->bcursor = 0;
    memset(printer->buffer, 0x00, sizeof(printer->buffer));
    
    // fprintf(printer->log, "Printer reset\n");
    timer_disable(&printer->pulse_timer);
    timer_stop(&printer->timeout_timer);
}


static
void push_byte(printer_t *printer)
{
    if(printer == NULL)
        return;
    printer->buffer[printer->bcursor++] = printer->data;
    if (printer->bcursor >= BUFFER_LEN) {
        // buffer is full
        flush_buffer(printer);
    }
}

static void
write_data(uint8_t val, void *priv)
{
    printer_t *dev = (printer_t *) priv;

    if (dev == NULL)
        return;

    dev->data = (char) val;
}

static void
strobe(uint8_t old, uint8_t val, void *priv)
{
    printer_t *dev = (printer_t *) priv;

    if (dev == NULL)
        return;

    if (!(val & 0x01) && (old & 0x01)) { /* STROBE */
        /* Process incoming character. */
        push_byte(dev);

        if (timer_is_on(&dev->timeout_timer)) {
            timer_stop(&dev->timeout_timer);
#ifdef USE_DYNAREC
            if (cpu_use_dynarec)
                update_tsc();
#endif
        }

        /* ACK it, will be read on next READ STATUS. */
        dev->ack = 1;

        timer_set_delay_u64(&dev->pulse_timer, ISACONST);
        timer_on_auto(&dev->timeout_timer, 5000000.0);
    }
}

static void
write_ctrl(uint8_t val, void *priv)
{
    printer_t *dev = (printer_t *) priv;

    if (dev == NULL)
        return;

    /* set autofeed value */
    dev->autofeed = val & 0x02 ? 1 : 0;

    if (val & 0x08) { /* SELECT */
        /* select printer */
        dev->select = 1;
    }

    if ((val & 0x04) && !(dev->ctrl & 0x04)) {
        /* reset printer */
        dev->select = 0;

        reset_printer(dev);
    }

    if (!(val & 0x01) && (dev->ctrl & 0x01)) { /* STROBE */
        /* Process incoming character. */
        push_byte(dev);

        /* ACK it, will be read on next READ STATUS. */
        dev->ack = 1;

        if (timer_is_on(&dev->timeout_timer)) {
            timer_stop(&dev->timeout_timer);
#ifdef USE_DYNAREC
            if (cpu_use_dynarec)
                update_tsc();
#endif
        }

        timer_set_delay_u64(&dev->pulse_timer, ISACONST);
        timer_on_auto(&dev->timeout_timer, 5000000.0);
    }

    dev->ctrl = val;
}

static uint8_t
read_status(void *priv)
{
    const printer_t *dev = (printer_t *) priv;
    uint8_t ret = 0x9f;

    if (!dev->ack)
        ret |= 0x40;

    return ret;
}

static void *
printer_init(const device_t *info)
{
    printer_t *printer = (printer_t *) malloc(sizeof(printer_t));
    if(printer == NULL)
        return printer;
    memset(printer, 0, sizeof(printer_t));

    /* Initialize parameters. */
    printer->ctrl = 0x04;
    printer->lpt  = lpt_attach(write_data, write_ctrl, strobe, read_status, NULL, NULL, NULL, printer);
    reset_printer(printer);

    // char filename[4096];
    // path_append_filename(filename, usr_path, "printer");
    // path_slash(filename);
    // strcat(filename, "printer.log");
    // printer->log = fopen(filename, "ab");
    // if(printer->log == NULL) {
    //     fprintf(stderr, "Can't open file %s\n", filename);
    //     goto ERROR;
    // }

    timer_add(&printer->pulse_timer, pulse_timer, printer, 0);
    timer_add(&printer->timeout_timer, timeout_timer, printer, 0);

    // fprintf(printer->log, "Printer initialized\n");

    return printer;

// ERROR:
//     if(printer != NULL)
//     {
//         if(printer->file != NULL)
//             fclose(printer->file);
//         // if(printer->log != NULL)
//         //     fclose(printer->log);
//         free(printer);
//         printer = NULL;
//     }
    // return NULL;
}

static void
printer_close(void *priv)
{
    printer_t *dev = (printer_t *) priv;

    if (dev == NULL)
        return;

    // fprintf(dev->log, "Printer closed\n");

    flush_buffer(dev);
    if(dev->file != NULL){
        fclose(dev->file);
        dev->file = NULL;
    }
    // fclose(dev->log);

    timer_disable(&dev->pulse_timer);
    timer_disable(&dev->timeout_timer);

    free(dev);
}

const device_t lpt_prt_tofile_device = {
    .name          = "Print To File",
    .internal_name = "prt_tofile",
    .flags         = DEVICE_LPT | DEVICE_HOTPLUG,
    .local         = 0,
    .init          = printer_init,
    .close         = printer_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};