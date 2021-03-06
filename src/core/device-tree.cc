/*
 * device-tree.cc
 *
 * This module parses the OpenFirmware device tree (published under /proc
 * by the kernel).
 *
 *
 *
 *
 */

#include <errno.h>
#include "version.h"
#include "device-tree.h"
#include "osutils.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

__ID("@(#) $Id$");

#define DIMMINFOSIZE 0x100
typedef __uint8_t dimminfo_buf[DIMMINFOSIZE];

struct dimminfo
{
  __uint8_t version3;
  char serial[16];
  __uint16_t version1, version2;
};

#define DEVICETREE "/proc/device-tree"

#define ntohll(x) (((uint64_t)(ntohl((uint32_t)((x << 32) >> 32))) << 32) | ntohl(((uint32_t)(x >> 32))))  

static unsigned long get_long(const string & path)
{
  unsigned long result = 0;
  int fd = open(path.c_str(), O_RDONLY);

  if (fd >= 0)
  {
    if(read(fd, &result, sizeof(result)) != sizeof(result))
      result = 0;

    close(fd);
  }

  if(sizeof(result) == sizeof(uint64_t))
    return ntohll(result);
  else
    return ntohl(result);
}


static vector < string > get_strings(const string & path,
unsigned int offset = 0)
{
  vector < string > result;
  char *strings = NULL;
  char *curstring = NULL;

  int fd = open(path.c_str(), O_RDONLY);

  if (fd >= 0)
  {
    struct stat buf;

    if (fstat(fd, &buf) == 0)
    {
      strings = (char *) malloc(buf.st_size + 1);
      if (strings)
      {
        memset(strings, 0, buf.st_size + 1);
        if(read(fd, strings, buf.st_size) == buf.st_size)
        {
          curstring = strings + offset;

          while (strlen(curstring))
          {
            result.push_back(string(curstring));
            curstring += strlen(curstring) + 1;
          }
        }

        free(strings);
      }
    }

    close(fd);
  }

  return result;
}


static void scan_devtree_root(hwNode & core)
{
  core.setClock(get_long(DEVICETREE "/clock-frequency"));
}


static void scan_devtree_bootrom(hwNode & core)
{
  if (exists(DEVICETREE "/rom/boot-rom"))
  {
    hwNode bootrom("firmware",
      hw::memory);
    string upgrade = "";
    unsigned long base = 0;
    unsigned long size = 0;

    bootrom.setProduct(get_string(DEVICETREE "/rom/boot-rom/model"));
    bootrom.setDescription("BootROM");
    bootrom.
      setVersion(get_string(DEVICETREE "/rom/boot-rom/BootROM-version"));

    if ((upgrade =
      get_string(DEVICETREE "/rom/boot-rom/write-characteristic")) != "")
    {
      bootrom.addCapability("upgrade");
      bootrom.addCapability(upgrade);
    }

    int fd = open(DEVICETREE "/rom/boot-rom/reg", O_RDONLY);
    if (fd >= 0)
    {
      if(read(fd, &base, sizeof(base)) == sizeof(base))
      {
        if(read(fd, &size, sizeof(size)) != sizeof(size))
          size = 0;
      }
      else
        base = 0;

      bootrom.setPhysId(base);
      bootrom.setSize(size);
      close(fd);
    }

    bootrom.claim();
//bootrom.setLogicalName(DEVICETREE "/rom");
    core.addChild(bootrom);
  }

  if (exists(DEVICETREE "/openprom"))
  {
    hwNode openprom("firmware",
      hw::memory);

    openprom.setProduct(get_string(DEVICETREE "/openprom/model"));

    if (exists(DEVICETREE "/openprom/supports-bootinfo"))
      openprom.addCapability("bootinfo");

//openprom.setLogicalName(DEVICETREE "/openprom");
    openprom.setLogicalName(DEVICETREE);
    openprom.claim();
    core.addChild(openprom);
  }
}


static string cpubusinfo(int cpu)
{
  char buffer[20];

  snprintf(buffer, sizeof(buffer), "cpu@%d", cpu);

  return string(buffer);
}


static void scan_devtree_cpu(hwNode & core)
{
  struct dirent **namelist;
  int n;
  int currentcpu=0;

  pushd(DEVICETREE "/cpus");
  n = scandir(".", &namelist, selectdir, alphasort);
  popd();
  if (n < 0)
    return;
  else
  {
    for (int i = 0; i < n; i++)
    {
      string basepath =
        string(DEVICETREE "/cpus/") + string(namelist[i]->d_name);
      unsigned long version = 0;
      hwNode cpu("cpu",
        hw::processor);
      struct dirent **cachelist;
      int ncache;

      if (exists(basepath + "/device_type") &&
        hw::strip(get_string(basepath + "/device_type")) != "cpu")
        break;                                    // oops, not a CPU!

      cpu.setProduct(get_string(basepath + "/name"));
      cpu.setDescription("CPU");
      cpu.claim();
      cpu.setBusInfo(cpubusinfo(currentcpu++));
      cpu.setSize(get_long(basepath + "/clock-frequency"));
      cpu.setClock(get_long(basepath + "/bus-frequency"));
      if (exists(basepath + "/altivec"))
        cpu.addCapability("altivec");

      version = get_long(basepath + "/cpu-version");
      if (version != 0)
      {
        int minor = version & 0x00ff;
        int major = (version & 0xff00) >> 8;
        char buffer[20];

        snprintf(buffer, sizeof(buffer), "%lx.%d.%d",
          (version & 0xffff0000) >> 16, major, minor);
        cpu.setVersion(buffer);

      }
      if (hw::strip(get_string(basepath + "/state")) != "running")
        cpu.disable();

      if (exists(basepath + "/performance-monitor"))
        cpu.addCapability("performance-monitor");

      if (exists(basepath + "/d-cache-size"))
      {
        hwNode cache("cache",
          hw::memory);

        cache.claim();
        cache.setDescription("L1 Cache");
        cache.setSize(get_long(basepath + "/d-cache-size"));
        if (cache.getSize() > 0)
          cpu.addChild(cache);
      }

      pushd(basepath);
      ncache = scandir(".", &cachelist, selectdir, alphasort);
      popd();
      if (ncache > 0)
      {
        for (int j = 0; j < ncache; j++)
        {
          hwNode cache("cache",
            hw::memory);
          string cachebase = basepath + "/" + cachelist[j]->d_name;

          if (hw::strip(get_string(cachebase + "/device_type")) != "cache" &&
            hw::strip(get_string(cachebase + "/device_type")) != "l2-cache")
            break;                                // oops, not a cache!

          cache.claim();
          cache.setDescription("L2 Cache");
          cache.setSize(get_long(cachebase + "/d-cache-size"));
          cache.setClock(get_long(cachebase + "/clock-frequency"));

          if (exists(cachebase + "/cache-unified"))
            cache.setDescription(cache.getDescription() + " (unified)");
          else
          {
            hwNode icache = cache;
            cache.setDescription(cache.getDescription() + " (data)");
            icache.setDescription(icache.getDescription() + " (instruction)");
            icache.setSize(get_long(cachebase + "/i-cache-size"));

            if (icache.getSize() > 0)
              cpu.addChild(icache);
          }

          if (cache.getSize() > 0)
            cpu.addChild(cache);

          free(cachelist[j]);
        }
        free(cachelist);
      }

      core.addChild(cpu);

      free(namelist[i]);
    }
    free(namelist);
  }
}

static void scan_devtree_memory_powernv(hwNode & core)
{
  struct dirent **namelist;
  hwNode *memory = core.getChild("memory");
  int n;

  pushd(DEVICETREE "/vpd");
  n = scandir(".", &namelist, selectdir, alphasort);
  popd();
  if (n < 0)
    return;
  for (int i = 0; i < n; i++)
  {
    string basepath;
    unsigned long size = 0;
    string sizestr;

    if (strncmp(namelist[i]->d_name, "ms-dimm@", 8) == 0)
    {
      hwNode bank("bank", hw::memory);

      if (!memory)
        memory = core.addChild(hwNode("memory", hw::memory));

      basepath = string(DEVICETREE "/vpd/") + string(namelist[i]->d_name);
      bank.setSerial(get_string(basepath + string("/serial-number")));
      bank.setProduct(get_string(basepath + string("/part-number")) + " FRU#" + get_string(basepath + string("/fru-number")));
      bank.setDescription(get_string(basepath + string("/description")));
      bank.setSlot(get_string(basepath + string("/ibm,loc-code")));
      sizestr = get_string(basepath + string("/size"));
      errno = 0;
      size = strtoul(sizestr.c_str(), NULL, 10);
      if (!errno)
        bank.setSize(size*1024*1024);
      bank.addHint("icon", string("memory"));

      memory->addChild(bank);
    }
    free(namelist[i]);
  }
  free(namelist);
}


static void scan_devtree_memory(hwNode & core)
{
  int currentmc = -1;                             // current memory controller
  hwNode *memory = core.getChild("memory");

  while (true)
  {
    char buffer[10];
    string mcbase;
    vector < string > slotnames;
    vector < string > dimmtypes;
    vector < string > dimmspeeds;
    string reg;
    string dimminfo;

    snprintf(buffer, sizeof(buffer), "%d", currentmc);
    if (currentmc >= 0)
      mcbase = string(DEVICETREE "/memory@") + string(buffer);
    else
      mcbase = string(DEVICETREE "/memory");
    slotnames =
      get_strings(mcbase + string("/slot-names"), sizeof(unsigned long));
    dimmtypes = get_strings(mcbase + string("/dimm-types"));
    dimmspeeds = get_strings(mcbase + string("/dimm-speeds"));
    reg = mcbase + string("/reg");
    dimminfo = mcbase + string("/dimm-info");

    if (slotnames.size() == 0)
    {
      if (currentmc < 0)
      {
        currentmc++;
        continue;
      }
      else
        break;
    }

    if (!memory || (currentmc > 0))
    {
      memory = core.addChild(hwNode("memory", hw::memory));
    }

    if (memory)
    {
      int fd = open(dimminfo.c_str(), O_RDONLY);
      int fd2 = open(reg.c_str(), O_RDONLY);

      if (fd2 >= 0)
      {
        for (unsigned int i = 0; i < slotnames.size(); i++)
        {
          uint64_t base = 0;
          uint64_t size = 0;
          hwNode bank("bank",
            hw::memory);

          if(read(fd2, &base, sizeof(base)) == sizeof(base))
          {
            if(read(fd2, &size, sizeof(size)) != sizeof(size))
              size = 0;
          }
          else
            base = 0;

          base = be64toh(base);
          size = be64toh(size);

          if (fd >= 0)
          {
            dimminfo_buf dimminfo;
	    
            if (read(fd, &dimminfo, 0x80) == 0x80)
            {

              /* Read entire SPD eeprom */
              if (dimminfo[2] >= 9) /* DDR3 */
              {
                read(fd, &dimminfo[0x80], (64 << ((dimminfo[0] & 0x70) >> 4)));
              } else if (dimminfo[0] < 15) { /* DDR 2 */
                read(fd, &dimminfo[0x80], (1 << (dimminfo[1]) ));
              }

              if (size > 0)
              {
                char dimmversion[20];
                unsigned char mfg_loc_offset;
                unsigned char rev_offset1;
                unsigned char rev_offset2;
                unsigned char year_offset;
                unsigned char week_offset;
                unsigned char partno_offset;
                unsigned char ver_offset;

                if (dimminfo[2] >= 9) {
                  mfg_loc_offset = 0x77;
                  rev_offset1 = 0x92;
                  rev_offset2 = 0x93;
                  year_offset = 0x78;
                  week_offset = 0x79;
                  partno_offset = 0x80;
                  ver_offset = 0x01;

                  switch ((dimminfo[0x8] >> 3) & 0x3) // DDR3 error detection and correction scheme
                  {
                    case 0x00:
                      bank.setConfig("errordetection", "none");
                      break;
                    case 0x01:
                      bank.addCapability("ecc");
                      bank.setConfig("errordetection", "ecc");
                      break;
                  }
                } else {
                  mfg_loc_offset = 0x48;
                  rev_offset1 = 0x5b;
                  rev_offset2 = 0x5c;
                  year_offset = 0x5d;
                  week_offset = 0x5e;
                  partno_offset = 0x49;
                  ver_offset = 0x3e;

                  switch (dimminfo[0xb] & 0x3) // DDR2 error detection and correction scheme
                  {
                    case 0x00:
                      bank.setConfig("errordetection", "none");
                      break;
                    case 0x01:
                      bank.addCapability("parity");
                      bank.setConfig("errordetection", "parity");
                      break;
                    case 0x02:
                    case 0x03:
                      bank.addCapability("ecc");
                      bank.setConfig("errordetection", "ecc");
                      break;
                  }
                }
                snprintf(dimmversion, sizeof(dimmversion),
                  "%02X%02X,%02X %02X,%02X", dimminfo[rev_offset1],
                  dimminfo[rev_offset2], dimminfo[year_offset], dimminfo[week_offset],
                  dimminfo[mfg_loc_offset]);
                bank.setSerial(string((char *) &dimminfo[partno_offset], 18));
                bank.setVersion(dimmversion);

                int version = dimminfo[ver_offset];
                char buff[32];

                snprintf(buff, sizeof(buff), "spd-%d.%d", (version & 0xF0) >> 4, version & 0x0F);
                bank.addCapability(buff);
              }
            }
          }

          if(size>0)
            bank.addHint("icon", string("memory"));
          bank.setDescription("Memory bank");
          bank.setSlot(slotnames[i]);
//bank.setPhysId(base);
          if (i < dimmtypes.size())
            bank.setDescription(dimmtypes[i]);
          if (i < dimmspeeds.size())
            bank.setProduct(hw::strip(dimmspeeds[i]));
          bank.setSize(size);
          memory->addChild(bank);
        }
        close(fd2);
      }

      if (fd >= 0)
        close(fd);
      currentmc++;
    }
    else
      break;

    memory = NULL;
  }
}


struct pmac_mb_def
{
  const char *model;
  const char *modelname;
  const char *icon;
};

static struct pmac_mb_def pmac_mb_defs[] =
{
/*
 * Warning: ordering is important as some models may claim
 * * beeing compatible with several types
 */
  {"AAPL,8500", "PowerMac 8500/8600", ""},
  {"AAPL,9500", "PowerMac 9500/9600", ""},
  {"AAPL,7200", "PowerMac 7200", ""},
  {"AAPL,7300", "PowerMac 7200/7300", ""},
  {"AAPL,7500", "PowerMac 7500", ""},
  {"AAPL,ShinerESB", "Apple Network Server", ""},
  {"AAPL,e407", "Alchemy", ""},
  {"AAPL,e411", "Gazelle", ""},
  {"AAPL,3400/2400", "PowerBook 3400", "laptop"},
  {"AAPL,3500", "PowerBook 3500", "laptop"},
  {"AAPL,Gossamer", "PowerMac G3 (Gossamer)", ""},
  {"AAPL,PowerMac G3", "PowerMac G3 (Silk)", ""},
  {"AAPL,PowerBook1998", "PowerBook Wallstreet", "laptop"},
  {"iMac,1", "iMac (first generation)", ""},
  {"PowerMac1,1", "Blue & White G3", "powermac"},
  {"PowerMac1,2", "PowerMac G4 PCI Graphics", "powermac"},
  {"PowerMac2,1", "iMac FireWire", ""},
  {"PowerMac2,2", "iMac FireWire", ""},
  {"PowerMac3,1", "PowerMac G4 AGP Graphics", "powermac"},
  {"PowerMac3,2", "PowerMac G4 AGP Graphics", "powermac"},
  {"PowerMac3,3", "PowerMac G4 AGP Graphics", "powermac"},
  {"PowerMac3,4", "PowerMac G4 QuickSilver", "powermac"},
  {"PowerMac3,5", "PowerMac G4 QuickSilver", "powermac"},
  {"PowerMac3,6", "PowerMac G4 Windtunnel", "powermac"},
  {"PowerMac4,1", "iMac \"Flower Power\"", ""},
  {"PowerMac4,2", "iMac LCD 15\"", ""},
  {"PowerMac4,4", "eMac", ""},
  {"PowerMac4,5", "iMac LCD 17\"", ""},
  {"PowerMac5,1", "PowerMac G4 Cube", ""},
  {"PowerMac5,2", "PowerMac G4 Cube", ""},
  {"PowerMac6,1", "iMac LCD 17\"", ""},
  {"PowerMac7,2", "PowerMac G5", "powermacg5"},
  {"PowerMac7,3", "PowerMac G5", "powermacg5"},
  {"PowerMac8,1", "iMac G5", ""},
  {"PowerMac8,2", "iMac G5", ""},
  {"PowerMac10,1", "Mac mini", "mini"},
  {"PowerMac10,2", "Mac mini", "mini"},
  {"PowerMac11,2", "PowerMac G5", "powermacg5"},
  {"PowerMac12,1", "iMac G5", ""},
  {"PowerBook1,1", "PowerBook 101 (Lombard)", "laptop"},
  {"PowerBook2,1", "iBook (first generation)", "laptop"},
  {"PowerBook2,2", "iBook FireWire", "laptop"},
  {"PowerBook3,1", "PowerBook Pismo", "laptop"},
  {"PowerBook3,2", "PowerBook Titanium", "laptop"},
  {"PowerBook3,3", "PowerBook Titanium w/ Gigabit Ethernet", "laptop"},
  {"PowerBook3,4", "PowerBook Titanium w/ DVI", "laptop"},
  {"PowerBook3,5", "PowerBook Titanium 1GHz", "laptop"},
  {"PowerBook4,1", "iBook 12\" (May 2001)", "laptop"},
  {"PowerBook4,2", "iBook 2", "laptop"},
  {"PowerBook4,3", "iBook 2 rev. 2 (Nov 2002)", "laptop"},
  {"PowerBook4,4", "iBook 2 rev. 2", "laptop"},
  {"PowerBook5,1", "PowerBook G4 17\"", "laptop"},
  {"PowerBook5,2", "PowerBook G4 15\"", "laptop"},
  {"PowerBook5,3", "PowerBook G4 17\" 1.33GHz", "laptop"},
  {"PowerBook5,4", "PowerBook G4 15\" 1.5/1.33GHz", "laptop"},
  {"PowerBook5,5", "PowerBook G4 17\" 1.5GHz", "laptop"},
  {"PowerBook5,6", "PowerBook G4 15\" 1.67/1.5GHz", "laptop"},
  {"PowerBook5,7", "PowerBook G4 17\" 1.67GHz", "laptop"},
  {"PowerBook5,8", "PowerBook G4 15\" double layer SD", "laptop"},
  {"PowerBook5,9", "PowerBook G4 17\" double layer SD", "laptop"},
  {"PowerBook6,1", "PowerBook G4 12\"", "laptop"},
  {"PowerBook6,2", "PowerBook G4 12\" DVI", "laptop"},
  {"PowerBook6,3", "iBook G4", "laptop"},
  {"PowerBook6,4", "PowerBook G4 12\"", "laptop"},
  {"PowerBook6,5", "iBook G4", "laptop"},
  {"PowerBook6,7", "iBook G4", "laptop"},
  {"PowerBook6,8", "PowerBook G4 12\" 1.5GHz", "laptop"},
  {"RackMac1,1", "XServe", ""},
  {"RackMac1,2", "XServe rev. 2", ""},
  {"RackMac3,1", "XServe G5", ""},
};

static bool get_apple_model(hwNode & n)
{
  string model = n.getProduct();
  if (model == "")
    return false;

  for (unsigned int i = 0; i < sizeof(pmac_mb_defs) / sizeof(pmac_mb_def);
    i++)
  if (model == pmac_mb_defs[i].model)
  {
    n.setProduct(pmac_mb_defs[i].modelname);
    n.addHint("icon", string(pmac_mb_defs[i].icon));
  }

  return false;
}


static void fix_serial_number(hwNode & n)
{
  string serial = n.getSerial();

  if(serial.find('\0')==string::npos) return;     // nothing to do

  n.setSerial(hw::strip(serial.substr(13)) + hw::strip(serial.substr(0,13)));
}


bool scan_device_tree(hwNode & n)
{
  hwNode *core = n.getChild("core");

  if (!exists(DEVICETREE))
    return false;

  if (!core)
  {
    n.addChild(hwNode("core", hw::bus));
    core = n.getChild("core");
  }

  n.setProduct(get_string(DEVICETREE "/model", n.getProduct()));
  n.addHint("icon", string("motherboard"));

  n.setSerial(get_string(DEVICETREE "/serial-number", n.getSerial()));
  if (n.getSerial() == "")
    n.setSerial(get_string(DEVICETREE "/system-id"));
  fix_serial_number(n);

  if (matches(get_string(DEVICETREE "/compatible"), "^ibm,powernv"))
  {
    n.setVendor(get_string(DEVICETREE "/vendor", "IBM"));
    n.setProduct(get_string(DEVICETREE "/model-name"));
    if (core)
    {
      core->addHint("icon", string("board"));
      scan_devtree_root(*core);
      scan_devtree_memory_powernv(*core);
      scan_devtree_cpu(*core);
      n.addCapability("powernv", "Non-virtualized");
      n.addCapability("opal", "OPAL firmware");
    }
  }
  else
  {
    n.setVendor(get_string(DEVICETREE "/copyright", n.getVendor()));
    get_apple_model(n);
    if (core)
    {
      core->addHint("icon", string("board"));
      scan_devtree_root(*core);
      scan_devtree_bootrom(*core);
      scan_devtree_memory(*core);
      scan_devtree_cpu(*core);
    }
  }

  return true;
}
