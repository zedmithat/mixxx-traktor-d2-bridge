#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int send_cc(snd_seq_t *seq, int port, int dest_client, int dest_port,
                   int channel, int control, int value) {
    snd_seq_event_t event;
    snd_seq_ev_clear(&event);
    snd_seq_ev_set_controller(&event, channel, control, value);
    snd_seq_ev_set_source(&event, port);
    snd_seq_ev_set_dest(&event, dest_client, dest_port);
    snd_seq_ev_set_direct(&event);
    return snd_seq_event_output_direct(seq, &event);
}

static int send_note(snd_seq_t *seq, int port, int dest_client, int dest_port,
                     int channel, int note, int velocity) {
    snd_seq_event_t event;
    snd_seq_ev_clear(&event);
    if (velocity)
        snd_seq_ev_set_noteon(&event, channel, note, velocity);
    else
        snd_seq_ev_set_noteoff(&event, channel, note, 0);
    snd_seq_ev_set_source(&event, port);
    snd_seq_ev_set_dest(&event, dest_client, dest_port);
    snd_seq_ev_set_direct(&event);
    return snd_seq_event_output_direct(seq, &event);
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 8) {
        fprintf(stderr,
                "usage: %s DEST_CLIENT DEST_PORT [C|N CHANNEL CONTROL VALUE RELEASE]\n",
                argv[0]);
        return 2;
    }
    snd_seq_t *seq = NULL;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0)
        return 1;
    snd_seq_set_client_name(seq, "D2 MIDI Probe");
    int port = snd_seq_create_simple_port(
        seq, "probe", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0)
        return 1;
    int client = atoi(argv[1]);
    int dest_port = atoi(argv[2]);
    int is_note = argc == 8 && argv[3][0] == 'N';
    int channel = argc == 8 ? atoi(argv[4]) : 1;
    int control = argc == 8 ? (int)strtol(argv[5], NULL, 0) : 0x14;
    int value = argc == 8 ? atoi(argv[6]) : 65;
    int release = argc == 8 ? atoi(argv[7]) : 63;
    int rc1 = is_note ?
        send_note(seq, port, client, dest_port, channel, control, value) :
        send_cc(seq, port, client, dest_port, channel, control, value);
    int rc2 = 0;
    if (release >= 0) {
        usleep(150000);
        rc2 = is_note ?
            send_note(seq, port, client, dest_port, channel, control, release) :
            send_cc(seq, port, client, dest_port, channel, control, release);
    }
    snd_seq_close(seq);
    printf("forward=%d backward=%d\n", rc1, rc2);
    return (rc1 < 0 || rc2 < 0) ? 1 : 0;
}
