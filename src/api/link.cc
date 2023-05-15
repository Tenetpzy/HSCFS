

namespace hscfs {

int link(const char *oldpath, const char *newpath)
{

}

}  // namespace hscfs


#ifdef CONFIG_C_API

extern "C" int link(const char *oldpath, const char *newpath)
{
    return hscfs::link(oldpath, newpath);
}

#endif