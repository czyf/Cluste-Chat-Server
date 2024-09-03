
#include "chatservice.h"
#include "public.h"
#include <muduo/base/Logging.h>
#include <vector>

using namespace std;
using namespace muduo;

// 获取单例对象的接口函数
ChatService *ChatService::instance()
{
    static ChatService service;
    return &service;
}

// 注册消息以及对应的Handle回调操作
ChatService::ChatService()
{
    _msgHandlerMap.insert({LOGIN_MSG, std::bind(&ChatService::login, this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG, std::bind(&ChatService::loginout, this, _1, _2, _3)});
    _msgHandlerMap.insert({REG_MSG, std::bind(&ChatService::reg, this, _1, _2, _3)});
    _msgHandlerMap.insert({ONE_CHAT_MES, std::bind(&ChatService::oneChat, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG, std::bind(&ChatService::addFriend, this, _1, _2, _3)});
    _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::creatGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG, std::bind(&ChatService::addGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG, std::bind(&ChatService::groupChat, this, _1, _2, _3)});

    // 连接redis服务器
    if (_redis.connect())
    {
        // 设置上报消息的回调
        _redis.init_notify_handler(std::bind(&ChatService::handleRedisSubscribeMessage, this, _1, _2));
    }
}

// 获取消息对应的处理器
MsgHandler ChatService::getHandler(int msgid)
{
    // 记录错误日志，msgid没有对应的事件处理回调
    auto it = _msgHandlerMap.find(msgid);
    if (it == _msgHandlerMap.end())
    {
        // 返回一个默认的处理器，空操作
        return [=](const TcpConnectionPtr &conn, json &js, Timestamp time)
        {
            LOG_ERROR << "msgid:" << msgid << " can not find hanadle!";
        };
    }
    else
        return _msgHandlerMap[msgid];
}

// 服务器异常，业务重置方法
void ChatService::reset()
{
    // 把online状态的用户重置为offline
    _userModel.resetState();
}

// 处理注销业务
void ChatService::loginout(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"];
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end())
        {
            _userConnMap.erase(it);
        }
    }

    // 用户注销，相当于就是下线，在redis中取消订阅通道
    _redis.unsubscribe(userid);

    // 更新用户的状态信息
    User user(userid, "", "", "offline");
    _userModel.updateState(user);
}

// 处理登录业务   id password
void ChatService::login(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    // LOG_INFO << "do login service!!!";

    int id = js["id"];
    string pwd = js["password"];

    User user = _userModel.query(id);

    if (user.getId() == id && user.getPassword() == pwd)
    {
        // 已登录后不能重复登录
        if (user.getState() == "online")
        {
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 2;
            response["errmsg"] = "该账号已登录";
            conn->send(response.dump());
        }
        else
        {
            // 登录成功，记录用户连接信息, 线程安全
            {
                lock_guard<mutex> lock(_connMutex);
                _userConnMap.insert({id, conn});
            }

            // id用户登录成功后，向redis订阅channel(id)
            _redis.subscribe(id);

            // 登录成功 更新用户状态信息 state offline->online
            user.setState("online");
            _userModel.updateState(user);

            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 0;
            response["id"] = user.getId();
            response["name"] = user.getName();

            // 查询该用户是否有离线消息
            vector<string> vec = _offlineMsgModel.query(id);
            if (!vec.empty())
            {
                response["offlinemsg"] = vec;
                // 读取该用户的离线消息后，把该用户的所有离线消息删除掉
                _offlineMsgModel.remove(id);
            }

            // 查询该用户的好友信息并返回
            vector<User> userVec = _friendModel.query(id);
            if (!userVec.empty())
            {
                vector<string> vec2;
                for (User &u : userVec)
                {
                    json js;
                    js["id"] = u.getId();
                    js["name"] = u.getName();
                    js["state"] = u.getState();
                    vec2.push_back(js.dump());
                }
                response["friends"] = vec2;
            }

            // 查询用户的群组消息
            vector<Group> groupuserVec = _groupModel.queryGroups(id);
            if (!groupuserVec.empty())
            {
                vector<string> groupV;
                for (Group &group : groupuserVec)
                {
                    json grpjson;
                    grpjson["id"] = group.getId();
                    grpjson["groupname"] = group.getName();
                    grpjson["groupdesc"] = group.getDesc();

                    vector<string> userVec;
                    for (GroupUser &user : group.getUsers())
                    {
                        json userjs;
                        userjs["id"] = user.getId();
                        userjs["name"] = user.getName();
                        userjs["state"] = user.getState();
                        userjs["role"] = user.getRole();
                        userVec.push_back(userjs.dump());
                    }
                    grpjson["users"] = userVec;
                    groupV.push_back(grpjson.dump());
                }

                response["groups"] = groupV;
            }

            conn->send(response.dump());
        }
    }
    else
    {
        // 用户不存在或密码错误
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "账户不存在或密码错误";
        conn->send(response.dump());
    }
}

// 处理注册业务  name  password
void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    LOG_INFO << "do reg service!!!";

    string name = js["name"];
    string password = js["password"];

    User user;
    user.setName(name);
    user.setPassword(password);
    bool state = _userModel.insert(user);
    if (state)
    {
        // 注册成功
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 0;
        response["id"] = user.getId();
        conn->send(response.dump());
    }
    else
    {
        // 注册失败
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        conn->send(response.dump());
    }
}

// 处理客户端异常退出clinetCloseException
void ChatService::clinetCloseException(const TcpConnectionPtr &conn)
{
    User user;
    {
        lock_guard<mutex> lock(_connMutex);

        for (auto it = _userConnMap.begin(); it != _userConnMap.end(); ++it)
        {
            if (it->second == conn)
            {
                // 从map表中删除用户连接信息
                user.setId(it->first);
                _userConnMap.erase(it);
                break;
            }
        }
    }

    // 用户注销，相当于就是下线，在redis中取消订阅通道
    _redis.unsubscribe(user.getId());

    // 更新用户在线状态信息
    if (user.getId() != -1)
    {
        user.setState("offline");
        _userModel.updateState(user);
    }
}
// 1对1聊天业务 msgid:业务代码 id:发送方id  from:发送方name to: 接收方id  msg: 消息
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int toId = js["toid"];

    {
        lock_guard<mutex> locj(_connMutex);
        auto it = _userConnMap.find(toId);
        if (it != _userConnMap.end()) // 在同一台机器上注册时 ,在在线表中查
        {
            // toid在线，转发id 服务器主动推送消息给toid接收方用户
            it->second->send(js.dump());
            return;
        }
    }

    // 不在同一台服务器上时，向其他服务器查询toid用户是否在线， 在数据库中查
    User user = _userModel.query(toId);
    if (user.getState() == "online")
    {
        _redis.publish(user.getId(), js.dump());
        return;
    }

    // toid不在线，存储离线消息
    _offlineMsgModel.insert(toId, js.dump());
}

// 添加好友业务  msgid:业务类型 id：发起方id，friendid：接收方id
void ChatService::addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"];
    int friendid = js["friendid"];

    // 存储好友消息
    _friendModel.insert(userid, friendid);
}

// 创建群组业务 mesgid，id，groupname， groupdesc
void ChatService::creatGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"];
    string groupName = js["groupname"];
    string groupDesc = js["groupdesc"];
    // 存储新创建的群组消息
    Group group(-1, groupName, groupDesc);
    if (_groupModel.createGroup(group))
    {
        // 创建成功后将该用户设置为creator
        _groupModel.addGroup(userid, group.getId(), "creator");
    }
}

// 加入群组业务
void ChatService::addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"];
    int groupid = js["groupid"];
    _groupModel.addGroup(userid, groupid, "normal");
}

// 群组聊天业务
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"];
    int groupid = js["groupid"];

    vector<int> groupUser = _groupModel.queryGroupUsers(userid, groupid);

    lock_guard<mutex> locl(_connMutex);
    for (int id : groupUser)
    {

        auto it = _userConnMap.find(id);

        if (it != _userConnMap.end())
        {
            // 该成员在线,转发群组消息
            it->second->send(js.dump());
        }
        else
        {
            // 查询toid是否在线
            User us = _userModel.query(id);
            if (us.getState() == "online")
            {
                _redis.publish(id, js.dump());
            }
            else
            {
                // 该成员不在线, 存储离线消息
                _offlineMsgModel.insert(id, js.dump());
            }
        }
    }
}

// 从redis消息队列中获取订阅的消息
void ChatService::handleRedisSubscribeMessage(int userid, string msg)
{
    lock_guard<mutex> lock(_connMutex);
    auto it = _userConnMap.find(userid);
    if (it != _userConnMap.end()){
        it->second->send(msg);
        return;
    }
    // 存储该用户的离线消息
    _offlineMsgModel.insert(userid, msg);
}