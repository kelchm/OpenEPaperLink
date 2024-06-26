#include <stdlib.h>
#include <unistd.h>
#include "RFQueue.h"
#include <stdio.h>
#include "ti_drivers_config.h"
#include <ti/drivers/GPIO.h>

#include "millis.h"
#include "uart.h"
#include "proto.h"
#include "radio.h"

// UART VARIABLES from uart.c
extern char input[];
extern size_t bytesReadCount;
// END UART VARIABLES from uart.c

// RADIO VARIABLES from radio.c
#define  max_rf_rx_buffer  20
extern uint8_t got_rx;
extern uint8_t worked_got_rx;
extern uint8_t packetDataPointer[max_rf_rx_buffer+1][1000];


uint8_t  mSelfMac[8] = {0x12,0x22,0x44,0x55,0x12,0x22,0x44,0x55};

#define LED1 10
#define LED2 11

// END RADIO VARIABLES from radio.c

///////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

const uint8_t channelList[6] = {11, 15, 20, 25, 26, 27};

bool radioTx(uint8_t* packet)
{
    timerDelay(2);
    radioSendData((uint8_t *)&packet[1], packet[0] - 2);
    return true;
}

void radioSetChannel(uint8_t ch)
{
    initRadio(ch);
}

void radioSetTxPower(uint8_t ch)
{

}

int8_t commsRxUnencrypted(uint8_t *data)
{
    if(got_rx != worked_got_rx)
    {
        uint8_t curr_len = packetDataPointer[worked_got_rx][0];
        GPIO_write(LED1,0);
        memcpy(data, (uint8_t*)&packetDataPointer[worked_got_rx][1], curr_len);
        worked_got_rx++;
        worked_got_rx %= max_rf_rx_buffer;
        GPIO_write(LED1,1);

        // Here we filter for our MAC Address to make sure nothing wrong gets trough
        struct MacFcs * fcs = (struct MacFcs *)data;
        if ((fcs->frameType == 1) && (fcs->destAddrType == 2) && (fcs->srcAddrType == 3) && (fcs->panIdCompressed == 0)) {
            // broadcast frame
            struct MacFrameBcast *rxframe = (struct MacFrameBcast *)data;
            if(rxframe->dstPan != PROTO_PAN_ID)
                return 0;
        } else if ((fcs->frameType == 1) && (fcs->destAddrType == 3) && (fcs->srcAddrType == 3) && (fcs->panIdCompressed == 1)) {
            // normal frame
            struct MacFrameNormal *txHeader = (struct MacFrameNormal *)data;
            if(memcmp(txHeader->dst,mSelfMac,8))
                return 0;
        }
        return curr_len - 2; // Remove appended RSSI and LQI, maybe its used later again
    }
    return 0;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

void uartTx(uint8_t data)
{
    uartWrite((uint8_t *)&data, 1);
}

void timerDelay(uint32_t ms)
{
    uint32_t nowTime = getMillis();
    while(getMillis()-nowTime<ms)
    {
    }

}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define DATATYPE_NOUPDATE 0
#define HW_TYPE 0xFF

#define MAX_PENDING_MACS 50
#define HOUSEKEEPING_INTERVAL 60UL

uint8_t  tempBuffer[320];

struct pendingData  pendingDataArr[MAX_PENDING_MACS];

// VERSION GOES HERE!
uint16_t  version = 0x0017;

#define RAW_PKT_PADDING 2

uint8_t  radiotxbuffer[128];
uint8_t  radiorxbuffer[128];

static uint32_t  housekeepingTimer;

struct blockRequest  requestedData = {0};  // holds which data was requested by the tag

uint8_t  dstMac[8];  // target for the block transfer
uint16_t  dstPan;    //

static uint32_t  blockStartTimer = 0;              // reference that holds when the AP sends the next block
uint32_t  nextBlockAttempt = 0;                    // reference time for when the AP can request a new block from the ESP32
uint8_t seq = 0;                                          // holds current sequence number for transmission
uint8_t  blockbuffer[BLOCK_XFER_BUFFER_SIZE + 5];  // block transfer buffer
uint8_t lastAckMac[8] = {0};

// these variables hold the current mac were talking to
#define CONCURRENT_REQUEST_DELAY 1200UL
uint32_t  lastBlockRequest = 0;
uint8_t  lastBlockMac[8];

uint8_t  curChannel = 11;
uint8_t  curPower = 10;

uint8_t  curPendingData = 0;
uint8_t  curNoUpdate = 0;

void sendXferCompleteAck(uint8_t *dst);
void sendCancelXfer(uint8_t *dst);
void espNotifyAPInfo();

// tools
void addCRC(void *p, uint8_t len) {
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++) {
        total += ((uint8_t *)p)[c];
    }
    ((uint8_t *)p)[0] = total;
    // printf("%d",total);
}
bool checkCRC(void *p, uint8_t len) {
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++) {
        total += ((uint8_t *)p)[c];
    }
    return ((uint8_t *)p)[0] == total;
}
void dump(uint8_t * a, uint16_t  l) {
    printf("        ");
#define ROWS 16
    for (uint8_t c = 0; c < ROWS; c++) {
        printf(" %02X", c);
    }
    printf("\n--------");
    for (uint8_t c = 0; c < ROWS; c++) {
        printf("---");
    }
    for (uint16_t c = 0; c < l; c++) {
        if ((c % ROWS) == 0) {
            printf("\n0x%04X | ", c);
        }
        printf("%02X ", a[c]);
    }
    printf("\n--------");
    for (uint8_t c = 0; c < ROWS; c++) {
        printf("---");
    }
    printf("\n");
}
uint8_t  getPacketType(void * buffer) {
    struct MacFcs * fcs = buffer;
    if ((fcs->frameType == 1) && (fcs->destAddrType == 2) && (fcs->srcAddrType == 3) && (fcs->panIdCompressed == 0)) {
        // broadcast frame
        uint8_t  type = ((uint8_t *)buffer)[sizeof(struct MacFrameBcast)];
        return type;
    } else if ((fcs->frameType == 1) && (fcs->destAddrType == 3) && (fcs->srcAddrType == 3) && (fcs->panIdCompressed == 1)) {
        // normal frame
        uint8_t  type = ((uint8_t *)buffer)[sizeof(struct MacFrameNormal)];
        return type;
    }
    return 0;
}
uint8_t getBlockDataLength() {
    uint8_t partNo = 0;
    for (uint8_t c = 0; c < BLOCK_MAX_PARTS; c++) {
        if (requestedData.requestedParts[c / 8] & (1 << (c % 8))) {
            partNo++;
        }
    }
    return partNo;
}

// pendingdata slot stuff
int8_t findSlotForMac(const uint8_t *mac) {
    for (uint8_t  c = 0; c < MAX_PENDING_MACS; c++) {
        // if (u64_isEq((uint64_t  *)mac, (uint64_t  *)&(pendingDataArr[c].targetMac))) {  // this costs 1 sloc :(
        if (memcmp(mac, ((uint8_t  *)&(pendingDataArr[c].targetMac)), 8) == 0) {
            if (pendingDataArr[c].attemptsLeft != 0) {
                return c;
            }
        }
    }
    return -1;
}
int8_t findFreeSlot() {
    for (uint8_t  c = 0; c < MAX_PENDING_MACS; c++) {
        if (pendingDataArr[c].attemptsLeft == 0) {
            return c;
        }
    }
    return -1;
}
int8_t findSlotForVer(const uint8_t *ver) {
    for (uint8_t  c = 0; c < MAX_PENDING_MACS; c++) {
        // if (u64_isEq((uint64_t  *)ver, (uint64_t  *)&(pendingDataArr[c].availdatainfo.dataVer))) {
        if (memcmp(ver, ((uint8_t  *)&(pendingDataArr[c].availdatainfo.dataVer)), 8) == 0) {
            if (pendingDataArr[c].attemptsLeft != 0) return c;
        }
    }
    return -1;
}
void deleteAllPendingDataForVer(const uint8_t *ver) {
    int8_t slot = -1;
    do {
        slot = findSlotForVer(ver);
        if (slot != -1) pendingDataArr[slot].attemptsLeft = 0;
    } while (slot != -1);
}
void deleteAllPendingDataForMac(const uint8_t *mac) {
    int8_t slot = -1;
    do {
        slot = findSlotForMac(mac);
        if (slot != -1) pendingDataArr[slot].attemptsLeft = 0;
    } while (slot != -1);
}

void countSlots() {
    curPendingData = 0;
    curNoUpdate = 0;
    for (uint8_t  c = 0; c < MAX_PENDING_MACS; c++) {
        if (pendingDataArr[c].attemptsLeft != 0) {
            if (pendingDataArr[c].availdatainfo.dataType != 0) {
                curPendingData++;
            } else {
                curNoUpdate++;
            }
        }
    }
}

// processing serial data
#define ZBS_RX_WAIT_HEADER 0
#define ZBS_RX_WAIT_SDA 1     // send data avail
#define ZBS_RX_WAIT_CANCEL 2  // cancel traffic for mac
#define ZBS_RX_WAIT_SCP 3     // set channel power

#define ZBS_RX_WAIT_BLOCKDATA 4

int blockPosition = 0;
void processSerial(uint8_t lastchar) {
    static uint8_t  cmdbuffer[4];
    static uint8_t  RXState = 0;
    static uint8_t  serialbuffer[48];
    static uint8_t * serialbufferp;
    static uint8_t  bytesRemain = 0;

    static uint32_t  lastSerial = 0;
    if ((getMillis() - lastSerial) > 25) {
        RXState = ZBS_RX_WAIT_HEADER;
        lastSerial = getMillis();
    } else {
        lastSerial = getMillis();
    }
    // uartTx(lastchar); echo
    switch (RXState) {
    case ZBS_RX_WAIT_HEADER:
        // shift characters in
        for (uint8_t c = 0; c < 3; c++) {
            cmdbuffer[c] = cmdbuffer[c + 1];
        }
        cmdbuffer[3] = lastchar;

        if (strncmp(cmdbuffer + 1, ">D>", 3) == 0) {
            printf("ACK>\n");
            blockPosition = 0;
            RXState = ZBS_RX_WAIT_BLOCKDATA;
        }

        if (strncmp(cmdbuffer, "SDA>", 4) == 0) {
            RXState = ZBS_RX_WAIT_SDA;
            bytesRemain = sizeof(struct pendingData);
            serialbufferp = serialbuffer;
            break;
        }
        if (strncmp(cmdbuffer, "CXD>", 4) == 0) {
            RXState = ZBS_RX_WAIT_CANCEL;
            bytesRemain = sizeof(struct pendingData);
            serialbufferp = serialbuffer;
            break;
        }
        if (strncmp(cmdbuffer, "SCP>", 4) == 0) {
            RXState = ZBS_RX_WAIT_SCP;
            bytesRemain = sizeof(struct espSetChannelPower);
            serialbufferp = serialbuffer;
            break;
        }
        if (strncmp(cmdbuffer, "NFO?", 4) == 0) {
            printf("ACK>");
            espNotifyAPInfo();
        }
        if (strncmp(cmdbuffer, "RDY?", 4) == 0) {
            printf("ACK>");
        }
        if (strncmp(cmdbuffer, "RSET", 4) == 0) {
            printf("ACK>");
            timerDelay(100);
            // TODO RESET US HERE
            RXState = ZBS_RX_WAIT_HEADER;
        }
        break;
    case ZBS_RX_WAIT_BLOCKDATA:
        blockbuffer[blockPosition++] = 0xAA ^ lastchar;
        if(blockPosition >= 4100){
            RXState = ZBS_RX_WAIT_HEADER;
        }
        break;

        case ZBS_RX_WAIT_SDA:
            *serialbufferp = lastchar;
            serialbufferp++;
            bytesRemain--;
            if (bytesRemain == 0) {
                if (checkCRC(serialbuffer, sizeof(struct pendingData))) {
                    struct pendingData *pd = (struct pendingData *)serialbuffer;
                        int8_t slot = findSlotForMac(pd->targetMac);
                        if (slot == -1) slot = findFreeSlot();
                        if (slot != -1) {
                            memcpy(&(pendingDataArr[slot]), serialbuffer, sizeof(struct pendingData));
                            printf("ACK>\n");
                        } else {
                            printf("NOQ>\n");
                        }
                } else {
                    printf("NOK>\n");
                }

                RXState = ZBS_RX_WAIT_HEADER;
            }
            break;
        case ZBS_RX_WAIT_CANCEL:
            *serialbufferp = lastchar;
            serialbufferp++;
            bytesRemain--;
            if (bytesRemain == 0) {
                if (checkCRC(serialbuffer, sizeof(struct pendingData))) {
                    struct pendingData *pd = (struct pendingData *)serialbuffer;
                        // deleteAllPendingDataForVer((uint8_t *)&pd->availdatainfo.dataVer);
                        deleteAllPendingDataForMac((uint8_t *)&pd->targetMac);
                        printf("ACK>\n");
                } else {
                    printf("NOK>\n");
                }

                RXState = ZBS_RX_WAIT_HEADER;
            }
            break;
        case ZBS_RX_WAIT_SCP:
            *serialbufferp = lastchar;
            serialbufferp++;
            bytesRemain--;
            if (bytesRemain == 0) {
                if (checkCRC(serialbuffer, sizeof(struct espSetChannelPower))) {
                    struct espSetChannelPower *scp = (struct espSetChannelPower *)serialbuffer;
                    for (uint8_t c = 0; c < sizeof(channelList); c++) {
                        if (channelList[c] == scp->channel) goto SCPchannelFound;
                    }
                    goto SCPfailed;
                SCPchannelFound:
                    curChannel = scp->channel;
                    curPower = scp->power;
                    radioSetChannel(scp->channel);
                    radioSetTxPower(scp->power);
                    printf("ACK>\n");
                } else {
                SCPfailed:
                    printf("NOK>\n");
                }
                RXState = ZBS_RX_WAIT_HEADER;
            }
            break;
    }
}

// sending data to the ESP
void espBlockRequest(const struct blockRequest *br, uint8_t *src) {
    struct espBlockRequest * ebr = (struct espBlockRequest *)blockbuffer;
    uartTx('R');
    uartTx('Q');
    uartTx('B');
    uartTx('>');
    // u64_copy(ebr->ver, br->ver);
    memcpy(&(ebr->ver), &(br->ver),8);
    memcpy(&(ebr->src), src,8);
    ebr->blockId = br->blockId;
    addCRC(ebr, sizeof(struct espBlockRequest));
    for (uint8_t c = 0; c < sizeof(struct espBlockRequest); c++) {
        uartTx(((uint8_t *)ebr)[c]);
    }
    // printf("req ebr ver: %02X%02X%02X%02X%02X%02X%02X%02X\n", ((uint8_t *)&(ebr->ver))[0], ((uint8_t *)&(ebr->ver))[1], ((uint8_t *)&(ebr->ver))[2], ((uint8_t *)&(ebr->ver))[3], ((uint8_t *)&(ebr->ver))[4], ((uint8_t *)&(ebr->ver))[5], ((uint8_t *)&(ebr->ver))[6], ((uint8_t *)&(ebr->ver))[7]);
    // printf("req br ver: %02X%02X%02X%02X%02X%02X%02X%02X\n", ((uint8_t *)&(br->ver))[0], ((uint8_t *)&(br->ver))[1], ((uint8_t *)&(br->ver))[2], ((uint8_t *)&(br->ver))[3], ((uint8_t *)&(br->ver))[4], ((uint8_t *)&(br->ver))[5], ((uint8_t *)&(br->ver))[6], ((uint8_t *)&(br->ver))[7]);
}
void espNotifyAvailDataReq(const struct AvailDataReq *adr, const uint8_t *src) {
    uartTx('A');
    uartTx('D');
    uartTx('R');
    uartTx('>');

    struct espAvailDataReq  eadr = {0};
    memcpy((void *)eadr.src, (void *)src, 8);
    memcpy((void *) & eadr.adr, (void *)adr, sizeof(struct AvailDataReq));
    addCRC(&eadr, sizeof(struct espAvailDataReq));
    for (uint8_t c = 0; c < sizeof(struct espAvailDataReq); c++) {
        uartTx(((uint8_t *)&eadr)[c]);
    }
}
void espNotifyXferComplete(const uint8_t *src) {
    struct espXferComplete exfc;
    memcpy(&exfc.src, src,8);
    uartTx('X');
    uartTx('F');
    uartTx('C');
    uartTx('>');
    addCRC(&exfc, sizeof(exfc));
    for (uint8_t c = 0; c < sizeof(exfc); c++) {
        uartTx(((uint8_t *)&exfc)[c]);
    }
}
void espNotifyTimeOut(const uint8_t *src) {
    struct espXferComplete exfc;
    memcpy(&exfc.src, src,8);
    uartTx('X');
    uartTx('T');
    uartTx('O');
    uartTx('>');
    addCRC(&exfc, sizeof(exfc));
    for (uint8_t c = 0; c < sizeof(exfc); c++) {
        uartTx(((uint8_t *)&exfc)[c]);
    }
}
void espNotifyAPInfo() {
    printf("TYP>%02X\n", HW_TYPE);
    printf("VER>%04X\n", version);
    printf("MAC>%02X%02X", mSelfMac[0], mSelfMac[1]);
    printf("%02X%02X", mSelfMac[2], mSelfMac[3]);
    printf("%02X%02X", mSelfMac[4], mSelfMac[5]);
    printf("%02X%02X\n", mSelfMac[6], mSelfMac[7]);
    printf("ZCH>%02X\n", curChannel);
    printf("ZPW>%02X\n", curPower);
    countSlots();
    printf("PEN>%02X\n", curPendingData);
    printf("NOP>%02X\n", curNoUpdate);
}

// process data from tag
void processBlockRequest(const uint8_t *buffer, uint8_t forceBlockDownload) {
    struct MacFrameNormal * rxHeader = (struct MacFrameNormal *)buffer;
    struct blockRequest * blockReq = (struct blockRequest *)(buffer + sizeof(struct MacFrameNormal) + 1);
    if (!checkCRC(blockReq, sizeof(struct blockRequest))) return;

    // check if we're already talking to this mac
    if (memcmp(rxHeader->src, lastBlockMac, 8) == 0) {
        lastBlockRequest = getMillis();
    } else {
        // we weren't talking to this mac, see if there was a transfer in progress from another mac, recently
        if ((getMillis() - lastBlockRequest) > CONCURRENT_REQUEST_DELAY) {
            // mark this mac as the new current mac we're talking to
            memcpy((void *)lastBlockMac, (void *)rxHeader->src, 8);
            lastBlockRequest = getMillis();
        } else {
            // we're talking to another mac, let this mac know we can't accomodate another request right now
            printf("BUSY!\n");
            sendCancelXfer(rxHeader->src);
            return;
        }
    }

    // check if we have data for this mac
    if (findSlotForMac(rxHeader->src) == -1) {
        // no data for this mac, politely tell it to fuck off
        sendCancelXfer(rxHeader->src);
        return;
    }

    bool  requestDataDownload = false;
    if ((blockReq->blockId != requestedData.blockId) || (blockReq->ver != requestedData.ver)) {
        // requested block isn't already in the buffer
        requestDataDownload = true;
    } else {
        // requested block is already in the buffer
        if (forceBlockDownload) {
            if ((getMillis() - nextBlockAttempt) > 380) {
                requestDataDownload = true;
                printf("FORCED\n");
            } else {
                printf("IGNORED\n");
            }
        }
    }

    // copy blockrequest into requested data
    memcpy(&requestedData, blockReq, sizeof(struct blockRequest));

    struct MacFrameNormal *txHeader = (struct MacFrameNormal *)(radiotxbuffer + 1);
    struct blockRequestAck *blockRequestAck = (struct blockRequestAck *)(radiotxbuffer + sizeof(struct MacFrameNormal) + 2);
    radiotxbuffer[0] = sizeof(struct MacFrameNormal) + 1 + sizeof(struct blockRequestAck) + RAW_PKT_PADDING;
    radiotxbuffer[sizeof(struct MacFrameNormal) + 1] = PKT_BLOCK_REQUEST_ACK;

    if (blockStartTimer == 0) {
        if (requestDataDownload) {
            // check if we need to download the first block; we need to give the ESP32 some additional time to cache the file
            if (blockReq->blockId == 0) {
                blockRequestAck->pleaseWaitMs = 400;
            } else {
                blockRequestAck->pleaseWaitMs = 400;
            }
        } else {
            // block is already in buffer
            blockRequestAck->pleaseWaitMs = 50;
        }
    } else {
        blockRequestAck->pleaseWaitMs = 50;
    }
    blockStartTimer = getMillis() + blockRequestAck->pleaseWaitMs;

    memcpy(txHeader->src, mSelfMac, 8);
    memcpy(txHeader->dst, rxHeader->src, 8);

    txHeader->pan = rxHeader->pan;
    txHeader->fcs.frameType = 1;
    txHeader->fcs.panIdCompressed = 1;
    txHeader->fcs.destAddrType = 3;
    txHeader->fcs.srcAddrType = 3;
    txHeader->seq = seq++;

    addCRC((void *)blockRequestAck, sizeof(struct blockRequestAck));

    radioTx(radiotxbuffer);

    // save the target for the blockdata
    memcpy(dstMac, rxHeader->src, 8);
    dstPan = rxHeader->pan;

    if (requestDataDownload) {
        blockPosition = 0;
        espBlockRequest(&requestedData, rxHeader->src);
        nextBlockAttempt = getMillis();
    }

    /*
        printf("Req: %d [", blockReq->blockId);
        for (uint8_t c = 0; c < BLOCK_MAX_PARTS; c++) {
            if ((c != 0) && (c % 8 == 0)) printf("][");
            if (blockReq->requestedParts[c / 8] & (1 << (c % 8))) {
                printf("R");
            } else {
                printf(".");
            }
        }
        printf("]\n");
        */
}

void processAvailDataReq(uint8_t *buffer) {
    struct MacFrameBcast *rxHeader = (struct MacFrameBcast *)buffer;
    struct AvailDataReq *availDataReq = (struct AvailDataReq *)(buffer + sizeof(struct MacFrameBcast) + 1);

    if (!checkCRC(availDataReq, sizeof(struct AvailDataReq)))
        return;

    // prepare tx buffer to send a response
    memset(radiotxbuffer, 0, sizeof(struct MacFrameNormal) + sizeof(struct AvailDataInfo) + 2);  // 120);
    struct MacFrameNormal *txHeader = (struct MacFrameNormal *)(radiotxbuffer + 1);
    struct AvailDataInfo *availDataInfo = (struct AvailDataInfo *)(radiotxbuffer + sizeof(struct MacFrameNormal) + 2);
    radiotxbuffer[0] = sizeof(struct MacFrameNormal) + 1 + sizeof(struct AvailDataInfo) + RAW_PKT_PADDING;
    radiotxbuffer[sizeof(struct MacFrameNormal) + 1] = PKT_AVAIL_DATA_INFO;

    // check to see if we have data available for this mac
    bool haveData = false;
    for (uint8_t  c = 0; c < MAX_PENDING_MACS; c++) {
        if (pendingDataArr[c].attemptsLeft) {
            if (memcmp(pendingDataArr[c].targetMac, rxHeader->src, 8) == 0) {
                haveData = true;
                memcpy((void *)availDataInfo, &(pendingDataArr[c].availdatainfo), sizeof(struct AvailDataInfo));
                break;
            }
        }
    }

    // couldn't find data for this mac
    if (!haveData) availDataInfo->dataType = DATATYPE_NOUPDATE;

    memcpy(txHeader->src, mSelfMac,8);
    memcpy(txHeader->dst, rxHeader->src,8);
    txHeader->pan = rxHeader->dstPan;
    txHeader->fcs.frameType = 1;
    txHeader->fcs.panIdCompressed = 1;
    txHeader->fcs.destAddrType = 3;
    txHeader->fcs.srcAddrType = 3;
    txHeader->seq = seq++;
    addCRC(availDataInfo, sizeof(struct AvailDataInfo));
    radioTx(radiotxbuffer);
    memset(lastAckMac, 0, 8);  // reset lastAckMac, so we can record if we've received exactly one ack packet
    espNotifyAvailDataReq(availDataReq, rxHeader->src);
}
void processXferComplete(uint8_t *buffer) {
    struct MacFrameNormal *rxHeader = (struct MacFrameNormal *)buffer;
    sendXferCompleteAck(rxHeader->src);
    if (memcmp(lastAckMac, rxHeader->src, 8) != 0) {
        memcpy((void *)lastAckMac, (void *)rxHeader->src, 8);
        espNotifyXferComplete(rxHeader->src);
        int8_t slot = findSlotForMac(rxHeader->src);
        if (slot != -1) pendingDataArr[slot].attemptsLeft = 0;
    }
}

// send block data to the tag
void sendPart(uint8_t partNo) {
    struct MacFrameNormal *frameHeader = (struct MacFrameNormal *)(radiotxbuffer + 1);
    struct blockPart *blockPart = (struct blockPart *)(radiotxbuffer + sizeof(struct MacFrameNormal) + 2);
    memset(radiotxbuffer + 1, 0, sizeof(struct blockPart) + sizeof(struct MacFrameNormal));
    radiotxbuffer[sizeof(struct MacFrameNormal) + 1] = PKT_BLOCK_PART;
    radiotxbuffer[0] = sizeof(struct MacFrameNormal) + sizeof(struct blockPart) + BLOCK_PART_DATA_SIZE + 1 + RAW_PKT_PADDING;
    memcpy(frameHeader->src, mSelfMac, 8);
    memcpy(frameHeader->dst, dstMac, 8);
    blockPart->blockId = requestedData.blockId;
    blockPart->blockPart = partNo;
    memcpy(&(blockPart->data), blockbuffer + (partNo * BLOCK_PART_DATA_SIZE), BLOCK_PART_DATA_SIZE);
    addCRC(blockPart, sizeof(struct blockPart) + BLOCK_PART_DATA_SIZE);
    frameHeader->fcs.frameType = 1;
    frameHeader->fcs.panIdCompressed = 1;
    frameHeader->fcs.destAddrType = 3;
    frameHeader->fcs.srcAddrType = 3;
    frameHeader->seq = seq++;
    frameHeader->pan = dstPan;
    radioTx(radiotxbuffer);
}
void sendBlockData() {
    if (getBlockDataLength() == 0) {
        printf("Invalid block request received, 0 parts..\n");
        requestedData.requestedParts[0] |= 0x01;
    }
    uint8_t partNo = 0;
    while (partNo < BLOCK_MAX_PARTS) {
        for (uint8_t c = 0; (c < BLOCK_MAX_PARTS) && (partNo < BLOCK_MAX_PARTS); c++) {
            if (requestedData.requestedParts[c / 8] & (1 << (c % 8))) {
                sendPart(c);
                partNo++;
            }
        }
    }
}
void sendXferCompleteAck(uint8_t *dst) {
    struct MacFrameNormal *frameHeader = (struct MacFrameNormal *)(radiotxbuffer + 1);
    memset(radiotxbuffer + 1, 0, sizeof(struct blockPart) + sizeof(struct MacFrameNormal));
    radiotxbuffer[sizeof(struct MacFrameNormal) + 1] = PKT_XFER_COMPLETE_ACK;
    radiotxbuffer[0] = sizeof(struct MacFrameNormal) + 1 + RAW_PKT_PADDING;
    memcpy(frameHeader->src, mSelfMac, 8);
    memcpy(frameHeader->dst, dst, 8);
    frameHeader->fcs.frameType = 1;
    frameHeader->fcs.panIdCompressed = 1;
    frameHeader->fcs.destAddrType = 3;
    frameHeader->fcs.srcAddrType = 3;
    frameHeader->seq = seq++;
    frameHeader->pan = dstPan;
    radioTx(radiotxbuffer);
}
void sendCancelXfer(uint8_t *dst) {
    struct MacFrameNormal *frameHeader = (struct MacFrameNormal *)(radiotxbuffer + 1);
    memset(radiotxbuffer + 1, 0, sizeof(struct blockPart) + sizeof(struct MacFrameNormal));
    radiotxbuffer[sizeof(struct MacFrameNormal) + 1] = PKT_CANCEL_XFER;
    radiotxbuffer[0] = sizeof(struct MacFrameNormal) + 1 + RAW_PKT_PADDING;
    memcpy(frameHeader->src, mSelfMac, 8);
    memcpy(frameHeader->dst, dst, 8);
    frameHeader->fcs.frameType = 1;
    frameHeader->fcs.panIdCompressed = 1;
    frameHeader->fcs.destAddrType = 3;
    frameHeader->fcs.srcAddrType = 3;
    frameHeader->seq = seq++;
    frameHeader->pan = dstPan;
    radioTx(radiotxbuffer);
}
void sendPong(void * buf) {
    struct MacFrameBcast *rxframe = (struct MacFrameBcast *)buf;
    struct MacFrameNormal *frameHeader = (struct MacFrameNormal *)(radiotxbuffer + 1);
    radiotxbuffer[sizeof(struct MacFrameNormal) + 1] = PKT_PONG;
    radiotxbuffer[sizeof(struct MacFrameNormal) + 2] = curChannel;
    radiotxbuffer[0] = sizeof(struct MacFrameNormal) + 1 + 1 + RAW_PKT_PADDING;
    memcpy(frameHeader->src, mSelfMac, 8);
    memcpy(frameHeader->dst, rxframe->src, 8);
    radiotxbuffer[1] = 0x41;  // fast way to set the appropriate bits
    radiotxbuffer[2] = 0xCC;  // normal frame
    frameHeader->seq = seq++;
    frameHeader->pan = rxframe->srcPan;
    radioTx(radiotxbuffer);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

void *mainThread(void *arg0)
{
    GPIO_setConfig(LED1, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    GPIO_write(LED1,1);
    GPIO_setConfig(LED2, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    GPIO_write(LED2,1);

    millisInit();
    initUart();

    requestedData.blockId = 0xFF;
    /*for (uint8_t c = 0; c < 8; c++) {
        mSelfMac[c] = c;
    }*/

    // clear the array with pending information
    memset(pendingDataArr, 0, sizeof(pendingDataArr));

    radioSetChannel(curChannel);
    radioSetTxPower(10);
    timerDelay(100);

    printf("RES>\n");
    printf("RDY>\n");

    housekeepingTimer = getMillis();

    uint16_t  loopCount = 1;
    while (1) {
        while ((getMillis() - housekeepingTimer) < ((1000 * HOUSEKEEPING_INTERVAL) - 100)) {
            int8_t ret = commsRxUnencrypted(radiorxbuffer);
            if (ret > 1) {
                // received a packet, lets see what it is
                switch (getPacketType(radiorxbuffer)) {
                    case PKT_AVAIL_DATA_REQ:
                        if (ret == 28) {
                            // old version of the AvailDataReq struct, set all the new fields to zero, so it will pass the CRC
                            processAvailDataReq(radiorxbuffer);
                            memset(radiorxbuffer + 1 + sizeof(struct MacFrameBcast) + sizeof(struct oldAvailDataReq), 0, sizeof(struct AvailDataReq) - sizeof(struct oldAvailDataReq) + 2);
                        } else if (ret == 40) {
                            // new version of the AvailDataReq struct
                            processAvailDataReq(radiorxbuffer);
                        }
                        break;
                    case PKT_BLOCK_REQUEST:
                        processBlockRequest(radiorxbuffer, 1);
                        break;
                    case PKT_BLOCK_PARTIAL_REQUEST:
                        processBlockRequest(radiorxbuffer, 0);
                        break;
                    case PKT_XFER_COMPLETE:
                        processXferComplete(radiorxbuffer);
                        break;
                    case PKT_PING:
                        sendPong(radiorxbuffer);
                        break;
                    case PKT_AVAIL_DATA_SHORTREQ:
                        // a short AvailDataReq is basically a very short (1 byte payload) packet that requires little preparation on the tx side, for optimal battery use
                        // bytes of the struct are set 0, so it passes the checksum test, and the ESP32 can detect that no interesting payload is sent
                        if (ret == 18) {
                            memset(radiorxbuffer + 1 + sizeof(struct MacFrameBcast), 0, sizeof(struct AvailDataReq) + 2);
                            processAvailDataReq(radiorxbuffer);
                        }
                        break;
                    default:
                        printf("t=%02X\n", getPacketType(radiorxbuffer));
                        break;
                }
                loopCount = 10000;
            }
            while(bytesReadCount){
                for(int i=0;i<bytesReadCount;i++)
                {
                    processSerial(input[i]);
                }
                bytesReadCount = 0;
                uartEnableInt();
            }
            if (blockStartTimer) {
                // BUG: uint32 overflowing; this will break every once in a while. Don't know how to fix this other than ugly global variables
                if (getMillis() > blockStartTimer) {
                    sendBlockData();
                    blockStartTimer = 0;
                }
            }
            loopCount--;
            if (loopCount == 0) {
                loopCount = 10000;
                // every once in a while, especially when handling a lot of traffic, the radio will hang. Calling this every once in while
                // alleviates this problem. The radio is set back to 'receive' whenever loopCount overflows
                //RADIO_command = RADIO_CMD_RECEIVE;
                // TODO Check if we also need to RX from time to time
            }
        }
        for (uint8_t  cCount = 0; cCount < MAX_PENDING_MACS; cCount++) {
            if (pendingDataArr[cCount].attemptsLeft == 1) {
                if (pendingDataArr[cCount].availdatainfo.dataType != DATATYPE_NOUPDATE) {
                    espNotifyTimeOut(pendingDataArr[cCount].targetMac);
                }
                pendingDataArr[cCount].attemptsLeft = 0;
            } else if (pendingDataArr[cCount].attemptsLeft > 1) {
                pendingDataArr[cCount].attemptsLeft--;
                if (pendingDataArr[cCount].availdatainfo.nextCheckIn) pendingDataArr[cCount].availdatainfo.nextCheckIn--;
            }
        }
        housekeepingTimer = getMillis();
    }
}
