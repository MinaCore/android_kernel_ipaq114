#ifndef __SGH_MSM6K__
#define __SGH_MSM6K__

#define CH_RPC	0

enum RPC_PKT_READ_TYPE {
	RPC_INDICATOR=1,
	RPC_RESPONSE,
	RPC_NOTIFICATION,
};

enum RPC_PKT_WRITE_TYPE {
	RPC_EXECUTE=1,
	RPC_GET,
	RPC_SET,
	RPC_CFRM,
};

#define RPC_GSM_CALL_INCOMING			0x0202
#define RPC_GSM_CALL_STATUS				0x0205

#define RPC_GSM_SEC_PIN_STATUS			0x0501
#define RPC_GSM_SEC_PHONE_LOCK			0x0502
#define RPC_GSM_SEC_LOCK_INFOMATION 	0x0508

#define RPC_GSM_SS_INFO					0x0c06

extern void smd_init(void);
extern int smd_read(int ch, void* buf, int len);
extern int smd_write(int ch, void *_buf, int len);
extern void smd_phone_power(int on);

extern void rpc_init(void);

#endif
