#pragma once

#include "stdint.h"

#define CHANNEL_COUNT   16
#define X_Axis_COUNT   2500
#define MAX_STATION    15     // 最大站点数（每个站对应一个IP）

typedef union {
	struct {
		bool v0 : 1;
		bool v1 : 1;
		bool v2 : 1;
		bool v3 : 1;

		bool v4 : 1;
		bool v5 : 1;
		bool v6 : 1;
		bool v7 : 1;
	}bit_v;
	int8_t value;
}BitUnion8;

typedef union {
	struct {
		int8_t u1;
		int8_t u2;
	}bit_v;
	int16_t value;
}BitUnion16;

typedef union {
	int nvalue;
	struct {
		uint8_t uv1;
		uint8_t uv2;
		uint8_t uv3;
		uint8_t uv4;
	}int8_v;
	struct {
		bool v0 : 1;
		bool v1 : 1;
		bool v2 : 1;
		bool v3 : 1;
		bool v4 : 1;
		bool v5 : 1;
		bool v6 : 1;
		bool v7 : 1;

		bool v8 : 1;
		bool v9 : 1;
		bool v10 : 1;
		bool v11 : 1;
		bool v12 : 1;
		bool v13 : 1;
		bool v14 : 1;
		bool v15 : 1;

		bool v16 : 1;
		bool v17 : 1;
		bool v18 : 1;
		bool v19 : 1;
		bool v20 : 1;
		bool v21 : 1;
		bool v22 : 1;
		bool v23 : 1;

		bool v24 : 1;
		bool v25 : 1;
		bool v26 : 1;
		bool v27 : 1;
		bool v28 : 1;
		bool v29 : 1;
		uint16_t r : 2;
	}bit_v;
}BitUnion32;



typedef enum
{
	E_UserRole_None = -1,        //未知
	E_UserRole_Admin = 2,        //管理员
	E_UserRole_Developer,       //开发者
	E_UserRole_Operator         //普通人员
}E_UserRole;

static E_UserRole GlobalLoginRole = E_UserRole_None;
static QString GlobalLoginUserName = "";
static QString GlobalLoginUserId = "";

struct ST_UserData
{
	E_UserRole role;
	int index = 0;
	int userId = 0;
	QString username;
	QString rulename;
	QString number;
	QString password;
	QString strrole;
};
Q_DECLARE_METATYPE(ST_UserData)

