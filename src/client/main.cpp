#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <semaphore.h>
#include <atomic>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "json.hpp"
#include "group.h"
#include "user.h"
#include "public.h"

using namespace std;
using json = nlohmann::json;

// 记录当前系统登录的用户信息
User g_currentUser;
// 记录当前登录用户的好友列表信息
vector<User> g_currentUserFriendList;
// 记录当前登录用户的群组列表消息
vector<Group> g_currentUserGroupList;
// 控制显示主菜单页面
bool isMainMenu = false;
// 显示当前登录成功用户的基本信息
void showCurrentUserData();

// 用于读写线程之间的通信
sem_t rwsem;
// 记录登录状态是否成功
atomic_bool g_isLoginSuccess = false;

// 接收线程
void readTaskHandler(int clientfd);
// 获取系统时间（聊天信息需要添加时间信息）
string getCurrentTime();
// 主聊天页面程序
void mainMenu(int clientfd);

// 聊天客户端程序实现，main线程用作发送线程，子线程用作接收线程
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        cerr << "command invalid! example: ./ChatClient 127.0.0.1 6000" << endl;
        exit(-1);
    }
    // 解析通过命令行参数传递的ip和port
    char *ip = argv[1];
    uint16_t port = atoi(argv[2]);

    // 创建client端的socket
    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientfd)
    {
        cerr << "socket create error" << endl;
        exit(-1);
    }

    // 填写client需要连接的server信息ip+port
    sockaddr_in server;
    memset(&server, 0, sizeof(sockaddr_in));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr((ip));

    // clint与server进行连接
    if (-1 == connect(clientfd, (sockaddr *)&server, sizeof(sockaddr_in)))
    {
        cerr << "connect server error" << endl;
        close(clientfd);
        exit(-1);
    }

    // 初始化读写线程通信用的信号量
    sem_init(&rwsem, 0, 0);

    // 登录成功 启动接收线程负责接收数据；
    std::thread readTask(readTaskHandler, clientfd);
    readTask.detach();

    // main线程用于接收用户输入，负责发送数据
    while (true)
    {
        // 显示首页面菜单 登录、注册、退出
        cout << "========================================" << endl;
        cout << "1. login" << endl;
        cout << "2. register" << endl;
        cout << "3. quit" << endl;
        cout << "========================================" << endl;
        cout << "choice: ";
        int choice = 0;
        cin >> choice;
        cin.get(); // 读掉缓冲区残留的回车

        // // 检查cin是否失败
        // if (std::cin.fail())
        // {
        //     std::cin.clear();                                                   // 重置cin的状态
        //     std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); // 清除输入缓冲区
        //     std::cout << "Invalid input. Please enter a number." << std::endl;
        // }
        // else
        // {
        //     std::cout << "You entered: " << choice << std::endl;
        // }

        switch (choice)
        {
        case 1: // login 业务
        {
            int id;
            char pwd[50] = {0};
            cout << "uderid:";
            cin >> id;
            cin.get();
            cout << "user password:";
            cin.getline(pwd, 50);

            json js;
            js["msgid"] = LOGIN_MSG;
            js["id"] = id;
            js["password"] = pwd;
            string request = js.dump();

            g_isLoginSuccess = false;
            int len = send(clientfd, request.c_str(), strlen(request.c_str()) + 1, 0);
            if (len == -1)
            {
                cerr << "send login msg error: " << request << endl;
            }

            // 等待信号量，由子线程处理完登录的响应消息后，通知这里
            sem_wait(&rwsem);
            if (g_isLoginSuccess)
            {
                // 进入聊天主界面
                isMainMenu = true;
                mainMenu(clientfd);
            }
        }
        break;
        case 2: // register业务
        {
            char name[50] = {0};
            char pwd[50] = {0};
            cout << "username:";
            cin.getline(name, 50);
            cout << "password:";
            cin.getline(pwd, 50);

            json js;
            js["msgid"] = REG_MSG;
            js["name"] = name;
            js["password"] = pwd;
            string request = js.dump();

            int len = send(clientfd, request.c_str(), strlen(request.c_str()) + 1, 0);
            if (len == -1)
            {
                cerr << "send reg msg error: " << request << endl;
            }
            // 等待信号量，由子线程处理完注册的响应消息后，通知这里
            sem_wait(&rwsem);
        }
        break;
        case 3: // quit业务
            close(clientfd);
            sem_destroy(&rwsem);
            exit(0);
        default:
            cerr << "invalid input" << endl;
            break;
        }
    }

    return 0;
}

// 显示当前登录成功用户的基本信息
void showCurrentUserData()
{
    cout << "=================================login user=================================" << endl;
    cout << "current login user -> id: " << g_currentUser.getId() << "  name: " << g_currentUser.getName() << endl;
    cout << "---------------------------------friend list-----------------------------------" << endl;

    if (!g_currentUserFriendList.empty())
    {
        for (User &user : g_currentUserFriendList)
        {
            cout << user.getId() << "  " << user.getName() << "  " << user.getState() << endl;
        }
    }

    cout << "---------------------------------group list-----------------------------------" << endl;

    if (!g_currentUserGroupList.empty())
    {
        for (Group &group : g_currentUserGroupList)
        {
            cout << group.getId() << "  " << group.getName() << "  " << group.getDesc() << endl;
            // 显示群好友
            for (GroupUser &user : group.getUsers())
            {
                cout << user.getId() << "  " << user.getName() << "  " << user.getState()
                     << user.getRole() << endl;
            }
        }
    }

    cout << "================================================================================" << endl;
}

// 处理登录的响应逻辑
void doLoginResponse(json &responsejs)
{
    if (responsejs["errno"] != 0)
    {
        //  登录失败
        cerr << responsejs["errmsg"] << endl;
        g_isLoginSuccess = false;
    }
    else
    {
        // 登录成功
        // 记录当前用户的id和name
        g_currentUser.setId(responsejs["id"]);
        g_currentUser.setName(responsejs["name"]);

        // 记录当前用户的好友列表信息
        if (responsejs.contains("friends"))
        {
            vector<string> vec = responsejs["friends"];
            for (string &str : vec)
            {
                json js = json::parse(str);
                User user;
                user.setId(js["id"]);
                user.setName(js["name"]);
                user.setState(js["state"]);
                g_currentUserFriendList.push_back(user);
            }
        }

        // 记录当前用户的群组列表信息
        if (responsejs.contains("groups"))
        {
            vector<string> vec = responsejs["groups"];
            for (string &groupstr : vec)
            {
                json js = json::parse(groupstr);
                Group group;
                group.setId(js["id"]);
                group.setName(js["groupname"]);
                group.setDesc(js["groupdesc"]);

                vector<string> vec2 = js["users"];
                for (string &userstr : vec2)
                {
                    json userjs = json::parse(userstr);
                    GroupUser user;
                    user.setId(userjs["id"]);
                    user.setName(userjs["name"]);
                    user.setState(userjs["state"]);
                    user.setRole(userjs["role"]);
                    group.getUsers().push_back(user);
                }

                g_currentUserGroupList.push_back(group);
            }
        }

        // 显示登录用户的基本信息
        showCurrentUserData();

        // 显示当前用户的离线消息 个人聊天消息或者群组消息
        if (responsejs.contains("offlinemsg"))
        {
            vector<string> vec = responsejs["offlinemsg"];
            for (string &str : vec)
            {
                json js = json::parse(str);
                if (ONE_CHAT_MES == js["msgid"])
                {
                    cout << js["time"] << " [" << js["id"] << "] " << js["name"] << " said: " << js["msg"] << endl;
                    continue;
                }
                else
                {
                    // 群消息
                    cout << "群消息[" << js["groupid"] << "]:" << js["time"] << " [" << js["id"] << "] " << js["name"] << " said: " << js["msg"] << endl;
                    continue;
                }
            }
        }
        g_isLoginSuccess = true;
    }
}

// 处理注册业务响应消息
void doRegResponse(json &reponsejs)
{
    if (reponsejs["errno"] != 0)
    {
        // 注册失败
        cerr << "name is already exist, register error!" << endl;
    }
    else
    {
        // 注册成功
        cout << "name register success, userid is " << reponsejs["id"] << ", do not forget it1" << endl;
    }
}

void readTaskHandler(int clientfd)
{
    while (true)
    {
        char buffer[1024] = {0};
        int len = recv(clientfd, buffer, 2014, 0); // 阻塞
        if (len == -1 || len == 0)
        {
            close(clientfd);
            exit(-1);
        }

        // 接收ChatServer转发的数据，反序列化生成json数据对象
        json js = json::parse(buffer);
        int msgid = js["msgid"];
        if (ONE_CHAT_MES == msgid)
        {
            cout << js["time"] << " [" << js["id"] << "] " << js["name"] << " said: " << js["msg"] << endl;
            continue;
        }
        if (GROUP_CHAT_MSG == msgid)
        {
            //
            cout << "群消息[" << js["groupid"] << "]:" << js["time"] << " [" << js["id"] << "] " << js["name"] << " said: " << js["msg"] << endl;
            continue;
        }
        if (LOGIN_MSG_ACK == msgid)
        {
            //
            doLoginResponse(js); // 处理登录响应的业务逻辑
            sem_post(&rwsem);    // 通知主线程，登录结果处理完成
            continue;
        }
        if (REG_MSG_ACK == msgid)
        {
            //
            doRegResponse(js); // 处理注册响应的业务逻辑
            sem_post(&rwsem);  // 通知主线程，注册结果处理完成
            continue;
        }
    }
}

// "help" command handler
void help(int fd = 0, string str = "");
// "chat" command handler
void chat(int fd = 0, string str = "");
// "addfriend" command handler
void addfriend(int fd = 0, string str = "");
// "creategroup" command handler
void creategroup(int fd = 0, string str = "");
// "addgroup" command handler
void addgroup(int fd = 0, string str = "");
// "groupchat" command handler
void groupchat(int fd = 0, string str = "");
// "loginout" command handler
void loginout(int fd = 0, string str = "");

// 系统支持的客户端命令列表
unordered_map<string, string> commandMap = {
    {"help", "显示所有支持的命令，格式help"},
    {"chat", "一对一聊天，格式chat:friend:message"},
    {"addfriend", "添加好友，格式addfriend:friendid"},
    {"creategroup", "创建群组，格式creategroup:groupname:groupdesc"},
    {"addgroup", "加入群组，格式addgroup:groupid"},
    {"groupchat", "群聊，格式groupchat:groupid:massage"},
    {"loginout", "注销，格式loginout"}};

// 注册系统支持的客户端命令处理
unordered_map<string, std::function<void(int, string)>> commandHandlerMap = {
    {"help", help},
    {"chat", chat},
    {"addfriend", addfriend},
    {"creategroup", creategroup},
    {"addgroup", addgroup},
    {"groupchat", groupchat},
    {"loginout", loginout}};

// 主聊天页面程序
void mainMenu(int clientfd)
{
    help();
    char buffer[1024] = {0};
    while (isMainMenu)
    {
        cin.getline(buffer, 1024);
        string commandbuf(buffer);
        string command; // 存储命令
        int idx = commandbuf.find(":");
        if (-1 == idx)
        {
            command = commandbuf;
        }
        else
        {
            command = commandbuf.substr(0, idx);
        }
        auto it = commandHandlerMap.find(command);
        if (it == commandHandlerMap.end())
        {
            cerr << "invalid input command" << endl;
            continue;
        }

        // 调用相应命令的事件处理回调，mainMenu对修改封闭，添加新功能不需要需改该函数
        it->second(clientfd, commandbuf.substr(idx + 1, commandbuf.size() - idx)); // 调用命令处理方法
    }
}

// "help" command handler
void help(int client, string str)
{
    cout << "show command list >>> " << endl;
    for (auto &p : commandMap)
    {
        cout << p.first << " : " << p.second << endl;
    }
    cout << endl;
}
// "addfriend" command handler
void addfriend(int clientfd, string str)
{

    int friendid = atoi(str.c_str());
    json js;
    js["msgid"] = ADD_FRIEND_MSG;
    js["id"] = g_currentUser.getId();
    js["friendid"] = friendid;
    string buffer = js.dump();

    int len = send(clientfd, buffer.c_str(), strlen(buffer.c_str()) + 1, 0);
    if (len == -1)
    {
        cerr << "send addfriend msg error -> " << buffer << endl;
    }
}
// "chat" command handler
void chat(int clientfd, string str)
{
    int idx = str.find(":"); // friendid:message
    if (-1 == idx)
    {
        cerr << "chat command invalid!" << endl;
        return;
    }

    int friendid = atoi(str.substr(0, idx).c_str());
    string msg = str.substr(idx + 1, str.size() - idx);

    json js;
    js["msgid"] = ONE_CHAT_MES;
    js["id"] = g_currentUser.getId();
    js["name"] = g_currentUser.getName();
    js["toid"] = friendid;
    js["msg"] = msg;
    js["time"] = getCurrentTime();
    string buffer = js.dump();

    int len = send(clientfd, buffer.c_str(), strlen(buffer.c_str()) + 1, 0);
    if (len == -1)
    {
        cerr << "send chat msg error -> " << buffer << endl;
    }
}
// "creategroup"
void creategroup(int clientfd, string str)
{
    int idx = str.find(":");
    if (idx == -1)
    {
        cerr << "creategroup command invalid!" << endl;
        return;
    }
    string groupname = str.substr(0, idx);
    string groupdesc = str.substr(idx + 1, str.size() - idx);

    json js;
    js["msgid"] = CREATE_GROUP_MSG;
    js["id"] = g_currentUser.getId();
    js["groupname"] = groupname;
    js["groupdesc"] = groupdesc;
    string buffer = js.dump();

    int len = send(clientfd, buffer.c_str(), strlen(buffer.c_str()) + 1, 0);
    if (len == -1)
    {
        cerr << "send creategroup msg error -> " << buffer << endl;
    }
}
// "addgroup"
void addgroup(int clientfd, string str)
{
    int groupid = atoi(str.c_str());
    json js;
    js["msgid"] = ADD_GROUP_MSG;
    js["id"] = g_currentUser.getId();
    js["groupid"] = groupid;
    string buffer = js.dump();

    int len = send(clientfd, buffer.c_str(), strlen(buffer.c_str()) + 1, 0);
    if (len == -1)
    {
        cerr << "send addgroup msg error -> " << buffer << endl;
    }
}
// "groupchat"
void groupchat(int clientfd, string str)
{
    int idx = str.find(":");
    if (idx == -1)
    {
        cerr << "groupchat command invalid!" << endl;
        return;
    }
    int groupid = atoi(str.substr(0, idx).c_str());
    string message = str.substr(idx + 1, str.size() - idx);

    json js;
    js["msgid"] = GROUP_CHAT_MSG;
    js["id"] = g_currentUser.getId();
    js["name"] = g_currentUser.getName();
    js["groupid"] = groupid;
    js["msg"] = message;
    js["time"] = getCurrentTime();
    string buffer = js.dump();

    int len = send(clientfd, buffer.c_str(), strlen(buffer.c_str()) + 1, 0);
    if (len == -1)
    {
        cerr << "send groupchat msg error -> " << buffer << endl;
    }
}
// "loginout"
void loginout(int clientfd, string str)
{
    json js;
    js["msgid"] = LOGINOUT_MSG;
    js["id"] = g_currentUser.getId();
    string buffer = js.dump();

    int len = send(clientfd, buffer.c_str(), strlen(buffer.c_str()) + 1, 0);
    if (len == -1)
    {
        cerr << "send loginout msg error -> " << buffer << endl;
    }
    else
    {
        isMainMenu = false;
        g_isLoginSuccess = false;
        g_currentUserFriendList.clear();
        g_currentUserGroupList.clear();
    }
}
// 获取系统时间（聊天信息需要添加时间信息）
string getCurrentTime()
{
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm *ptm = localtime(&tt);
    char date[60] = {0};
    sprintf(date, "%d-%02d-%02d %02d:%02d:%02d",
            (int)ptm->tm_year + 1900, (int)ptm->tm_mon + 1, (int)ptm->tm_mday,
            (int)ptm->tm_hour, (int)ptm->tm_min, (int)ptm->tm_sec);
    return std::string(date);
}
