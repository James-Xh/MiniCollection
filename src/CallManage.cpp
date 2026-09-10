#include "CallManage.h"


CallManage::CallManage(QObject *parent) : QObject(parent)
{

}

CallManage::~CallManage()
{
	
}

CallManage *CallManage::getInstance()
{
	static CallManage ref;
	return &ref;
}
