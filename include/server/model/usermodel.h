#ifndef USERMODEL_H
#define USERMODEL_H

#include"user.h"

// User表的数据操作类
class UserModel{
public:
    // User增加方法
    bool insert(User &user);
    // User查询方法
    User query(int id);
    // 更新用户的状态信息
    bool updateState(User user);
    // 重置用户的在线状态信息
    void resetState();
private:

};

#endif