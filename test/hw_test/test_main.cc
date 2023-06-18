#include "api/hscfs.hh"

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <memory>
#include <cassert>
using namespace std;

class Test_Env
{
public:

    Test_Env(int argc, char *argv[]) {
        if (hscfs::init(argc, argv) == -1)
            throw std::runtime_error("hscfs init failed.");
    }

    ~Test_Env() {
        hscfs::fini();
    }

    void add_opened_file(int fd, const string &path)
    {
        fd_path_map[fd] = path;
    }

    void close_file(int fd)
    {
        fd_path_map.erase(fd);
    }

    void print_current_fds() const
    {
        cout << "fd : pathname\n";
        for (auto &entry: fd_path_map)
            cout << entry.first << " : " << entry.second << '\n';
    }

private:
    unordered_map<int, string> fd_path_map;
};

class Error_Handler
{
public:
    Error_Handler(const char *msg) {
        this->msg = msg;
    }

    void operator()()
    {
        int err = errno;
        perror(msg);
        if (err == ENOTRECOVERABLE)
            exit(1);
    }

private:
    const char *msg;
};

class Ihandler
{
public:
    Ihandler(Test_Env *env) {
        test_env = env;
    }

    virtual ~Ihandler() = default;

    virtual void operator()() = 0;

protected:
    Test_Env *test_env;
};

enum Handler_ID {
    create = 0, open = 1, close = 2, read = 3, write = 4, trunc = 5, fsync = 6, unlink = 7, link = 8, mkdir = 9, rmdir = 10,
    print_fd = 11, quit = 12
};

/*
 * args: /path/to/file
 */
class Create_Handler: public Ihandler
{
public:
    Create_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        string path;
        cin >> path;
        int fd = hscfs::open(path.c_str(), O_RDWR | O_CREAT);
        if (fd == -1)
            Error_Handler("open")();
        else
        {
            cout << "fd of file " << path << ": " << fd << endl;
            test_env->add_opened_file(fd, path);
        }
    }
};

/*
 * args: /path/to/file
 */
class Open_Handler: public Ihandler
{
public:
    Open_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        string path;
        cin >> path;
        int fd = hscfs::open(path.c_str(), O_RDWR);
        if (fd == -1)
            Error_Handler("open")();
        else
        {
            cout << "fd of file " << path << ": " << fd << endl;
            test_env->add_opened_file(fd, path);
        }
    }
};

/*
 * args: fd
 */
class Close_Handler: public Ihandler
{
public:
    Close_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        int fd;
        cin >> fd;
        int ret = hscfs::close(fd);
        if (ret == -1)
            Error_Handler("close")();
        else
            test_env->close_file(fd);
    }
};

/*
 * args: fd outputfile
 */
class Read_Handler: public Ihandler
{
public:
    Read_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        int fd;
        string output_file;
        cin >> fd >> output_file;
        ofstream of(output_file);
        auto buf = std::unique_ptr<char[]>(new char[512]);
        ssize_t cnt;

        while (true)
        {
            cnt = hscfs::read(fd, buf.get(), 512);
            if (cnt == 0)
                break;
            if (cnt == -1)
            {
                Error_Handler("read")();
                break;
            }
            of.write(buf.get(), cnt);
        }
    }
};

/*
 * args: fd inputfile
 */
class Write_Handler: public Ihandler
{
public:
    Write_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        int fd;
        string input_file;
        cin >> fd >> input_file;
        ifstream input(input_file);
        string content((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
        ssize_t cnt;
        
        cnt = hscfs::write(fd, const_cast<char*>(content.c_str()), content.length());
        if (cnt == -1)
            Error_Handler("write")();
        else
            assert((size_t)cnt == content.length());
    }
};

/*
 * args: fd length
 */
class Trunc_Handler: public Ihandler
{
public:
    Trunc_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        int fd;
        off_t length;
        cin >> fd >> length;
        int ret = hscfs::truncate(fd, length);
        if (ret == -1)
            Error_Handler("trunc")();
    }
};

/* args: fd */
class Fsync_Handler: public Ihandler
{
public:
    Fsync_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        int fd;
        cin >> fd;
        int ret = hscfs::fsync(fd);
        if (ret == -1)
            Error_Handler("fsync")();
    }
};

/* args: pathname */
class Unlink_Handler: public Ihandler
{
public:
    Unlink_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        string path;
        cin >> path;
        if (hscfs::unlink(path.c_str()) == -1)
            Error_Handler("unlink")();
    }
};

/* args: oldpath newpath */
class Link_Handler: public Ihandler
{
public:
    Link_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        string oldpath, newpath;
        cin >> oldpath >> newpath;
        if (hscfs::link(oldpath.c_str(), newpath.c_str()) == -1)
            Error_Handler("link")();
    }
};

/* args: pathname */
class Mkdir_Handler: public Ihandler
{
public:
    Mkdir_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        string path;
        cin >> path;
        if (hscfs::mkdir(path.c_str()) == -1)
            Error_Handler("mkdir")();
    }
};

/* args: pathname */
class Rmdir_Handler: public Ihandler
{
public:
    Rmdir_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        string path;
        cin >> path;
        if (hscfs::rmdir(path.c_str()) == -1)
            Error_Handler("rmdir")();
    }
};

/* args(None) */
class Print_Fd_Handler: public Ihandler
{
public:
    Print_Fd_Handler(Test_Env *env): Ihandler(env) {}

    void operator()() override
    {
        test_env->print_current_fds();
    }
};

class Handler_Factory
{
public:
    Handler_Factory(Test_Env *env) {
        this->env = env;
    }

    unique_ptr<Ihandler> get_handler(int id)
    {
        return map[id](env);
    }

private:
    static unique_ptr<Ihandler> create_constructor(Test_Env *env) {
        return make_unique<Create_Handler>(env);
    }
    static unique_ptr<Ihandler> open_constructor(Test_Env *env) {
        return make_unique<Open_Handler>(env);
    }
    static unique_ptr<Ihandler> close_constructor(Test_Env *env) {
        return make_unique<Close_Handler>(env);
    }
    static unique_ptr<Ihandler> read_constructor(Test_Env *env) {
        return make_unique<Read_Handler>(env);
    }
    static unique_ptr<Ihandler> write_constructor(Test_Env *env) {
        return make_unique<Write_Handler>(env);
    }
    static unique_ptr<Ihandler> trunc_constructor(Test_Env *env) {
        return make_unique<Trunc_Handler>(env);
    }
    static unique_ptr<Ihandler> fsync_constructor(Test_Env *env) {
        return make_unique<Fsync_Handler>(env);
    }
    static unique_ptr<Ihandler> unlink_constructor(Test_Env *env) {
        return make_unique<Unlink_Handler>(env);
    }
    static unique_ptr<Ihandler> link_constructor(Test_Env *env) {
        return make_unique<Link_Handler>(env);
    }
    static unique_ptr<Ihandler> mkdir_constructor(Test_Env *env) {
        return make_unique<Mkdir_Handler>(env);
    }
    static unique_ptr<Ihandler> rmdir_constructor(Test_Env *env) {
        return make_unique<Rmdir_Handler>(env);
    }
    static unique_ptr<Ihandler> print_fd_constructor(Test_Env *env) {
        return make_unique<Print_Fd_Handler>(env);
    }

private:
    Test_Env *env;

    typedef unique_ptr<Ihandler>(*constructor_t)(Test_Env *);
    unordered_map<int, constructor_t> map = {
        {Handler_ID::create, create_constructor},
        {Handler_ID::open, open_constructor}, 
        {Handler_ID::close, close_constructor},
        {Handler_ID::read, read_constructor},
        {Handler_ID::write, write_constructor},
        {Handler_ID::trunc, trunc_constructor},
        {Handler_ID::fsync, fsync_constructor},
        {Handler_ID::unlink, unlink_constructor},
        {Handler_ID::link, link_constructor},
        {Handler_ID::mkdir, mkdir_constructor},
        {Handler_ID::rmdir, rmdir_constructor},
        {Handler_ID::print_fd, print_fd_constructor}
    };
};

void print_help_msg()
{
    cout << endl;
    cout << "input <handlerID> <args>... to call each function below:\n"
    << "handlerID: handlerName arg1 arg2...\n"
    << Handler_ID::create << ": create path\n"
    << Handler_ID::open << ": open path\n"
    << Handler_ID::close << ": close fd\n"
    << Handler_ID::read << ": read fd output_file_name\n"
    << Handler_ID::write << ": write fd input_file_name\n"
    << Handler_ID::trunc << ": trunc fd length\n"
    << Handler_ID::fsync << ": fsync fd\n"
    << Handler_ID::unlink << ": unlink path\n"
    << Handler_ID::link << ": link oldpath newpath\n"
    << Handler_ID::mkdir << ": mkdir path\n"
    << Handler_ID::rmdir << ": rmdir path\n"
    << Handler_ID::print_fd << ": print opened fd and file name\n"
    << Handler_ID::quit << ": exit\n";
}

int main(int argc, char *argv[])
{
    Test_Env env(argc, argv);
    Handler_Factory factory(&env);

    int handler_id;
    while (true)
    {
        print_help_msg();
        cin >> handler_id;
        if (handler_id == Handler_ID::quit)
            break;
        factory.get_handler(handler_id)->operator()();
    }

    return 0;
}