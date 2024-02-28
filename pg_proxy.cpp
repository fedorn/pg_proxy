#include <sys/epoll.h>
#include <netinet/in.h>
#include <unordered_map>
#include <iostream>
#include <map>
#include <string>
#include <sstream>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
// Other necessary includes...

int set_nonblock(int fd)
{
    int flags;
#if defined(O_NONBLOCK)
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    flags = 1;
    return ioctl(fd, FIOBIO, &flags);
#endif
}

class PostgreSQLProxy
{
private:
    int listen_fd;                                 // Listening socket file descriptor
    int efd;                                       // Epoll file descriptor
    std::unordered_map<int, int> client_to_server; // Map client FD to server FD
    std::unordered_map<int, int> server_to_client; // Map server FD to client FD

public:
    PostgreSQLProxy(int port);
    ~PostgreSQLProxy();
    void run();
    void handleNewConnection();
    void forwardData(int fd);
    // Other methods...
};

PostgreSQLProxy::PostgreSQLProxy(int port)
{
    // Initialize listening socket and epoll
    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    // Создание описания адреса для этого сокета
    struct sockaddr_in sockAddr;
    sockAddr.sin_family = AF_INET;
    sockAddr.sin_port = htons(port);
    sockAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Связывание сокета и адреса
    if (bind(listen_fd, reinterpret_cast<struct sockaddr *>(&sockAddr), sizeof(sockAddr)) == -1)
    {
        close(listen_fd);
        throw std::runtime_error("Bind failed");
    }

    // Перевод сокета в неблокирующий режим
    set_nonblock(listen_fd);

    // Перевод сокета в режим приема подключений
    if (listen(listen_fd, SOMAXCONN) == -1)
    {
        close(listen_fd);
        throw std::runtime_error("Listen failed");
    }

    // Работа с epoll
    struct epoll_event event;  // событие
    event.data.fd = listen_fd; // сокет
    event.events = EPOLLIN;    // тип события

    // создание дескриптора epoll
    efd = epoll_create1(0);
    if (efd == -1)
    {
        close(listen_fd);
        throw std::runtime_error("Epoll create failed");
    }
    epoll_ctl(efd, EPOLL_CTL_ADD, listen_fd, &event); // добавление событя
}

PostgreSQLProxy::~PostgreSQLProxy()
{
    close(efd);
    close(listen_fd);
}

void PostgreSQLProxy::run()
{
    // Теперь можно принимать и обрабатывать соединения
    static const int maxEvents = 32;
    while (true)
    {
        struct epoll_event events[maxEvents];               // массив для событий
        int count = epoll_wait(efd, events, maxEvents, -1); // получение событий

        // Разбор полученных событий
        for (int i = 0; i < count; ++i)
        {
            struct epoll_event &e = events[i];

            // Пришло событие от основного сокета
            if (e.data.fd == listen_fd)
            {
                handleNewConnection();
            }
            // Пришло событие от сокета клиентов или postgres
            else
            {
                forwardData(e.data.fd);
            }
        }
    }
}

void PostgreSQLProxy::handleNewConnection()
{
    // Accept new connection and add to epoll
    struct sockaddr_in newAddr;
    socklen_t length = sizeof(newAddr);
    // Принятие соединения
    int newSocket = accept(listen_fd,
                           reinterpret_cast<sockaddr *>(&newAddr),
                           &length);
    if (newSocket == -1)
    {
        throw std::runtime_error("Accept client failed");
    }
    set_nonblock(newSocket); // перевод в неблокирующий режим

    // Добавление сокета в epoll
    struct epoll_event event;
    event.data.fd = newSocket;
    event.events = EPOLLIN;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, newSocket, &event) == -1)
    {
        throw std::runtime_error("Epoll add failed");
    }

    std::cout << "client " << newSocket << " connected." << std::endl;

    // Добавление нового клиента
    int pgSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in pgAddr
    {
    };
    pgAddr.sin_family = AF_INET;
    pgAddr.sin_port = htons(5432);
    pgAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(pgSock, (struct sockaddr *)&pgAddr, sizeof(pgAddr)) == -1)
    {
        throw std::runtime_error("Connection to PostgreSQL failed");
    }
    set_nonblock(pgSock); // перевод в неблокирующий режим

    // Добавление сокета в epoll
    struct epoll_event pgEvent;
    pgEvent.data.fd = pgSock;
    pgEvent.events = EPOLLIN;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, pgSock, &pgEvent) == -1)
    {
        throw std::runtime_error("Epoll add failed");
    }

    client_to_server[newSocket] = pgSock;
    server_to_client[pgSock] = newSocket;
}

void PostgreSQLProxy::forwardData(int fd)
{
    // Read data from fd, log it, and forward to the corresponding FD
    // Длина и буфер для приема
    static const size_t length = 1024;
    static char buffer[length];

    // Получение
    int result = recv(fd, buffer, length - 1, MSG_NOSIGNAL);

    // Клиент отключился
    if (result == 0 && errno != EAGAIN)
    {
        // Закрытие соединения
        shutdown(fd, SHUT_RDWR);
        close(fd);

        // Создание текста сообщения об отключении
        std::stringstream str;
        std::cout << fd << " disconnected." << std::endl;

        // Удаление клиента из контейнера
        // clients.erase(e.data.fd);
    }
    // Клиент прислал данные
    else if (result > 0)
    {
        if (client_to_server.find(fd) != client_to_server.end())
        {
            std::cout << "send to server" << client_to_server[fd] << "from client " << fd << ' ' << result << " bytes : " << buffer << std::endl;
            send(client_to_server[fd], buffer, result, MSG_NOSIGNAL);
        }
        else if (server_to_client.find(fd) != server_to_client.end())
        {
            std::cout << "send to client" << server_to_client[fd] << "from server " << fd << ' ' << result << " bytes : " << buffer << std::endl;
            send(server_to_client[fd], buffer, result, MSG_NOSIGNAL);
        }
        else
        {
            throw std::runtime_error("Unknown descriptor");
        }

        // Дописывание буфера для клиента
        // buffer[result] = 0;

        // Поиск завершения строки
        // size_t position = c.Message.find("\r\n");
        // if (position != std::string::npos)
        // {
        //     std::string message = c.Message.substr(0, position + 2);

        //     std::stringstream str;
        //     // str << c.Name << ": " << message;
        //     str << message;

        //     c.Message.erase(0, position + 2);

        //     for (auto i = clients.begin();
        //          i != clients.end(); ++i)
        //     {
        //         send(i->first, str.str().data(), str.str().length(), MSG_NOSIGNAL);
        //     }
        // }
    }
}

int main()
{
    PostgreSQLProxy proxy(5434); // Default PostgreSQL port
    proxy.run();
    return 0;
}
