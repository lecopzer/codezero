#! /usr/bin/env python2.6
# -*- mode: python; coding: utf-8; -*-
import os, sys, shelve, shutil, re
from projpaths import *

class Container:
    def __init__(self, id):
        self.dirname = None
        self.name = None
        self.type = None
        self.id = id
        self.pager_lma = 0
        self.pager_vma = 0
        self.pager_size = 0
        self.linux_page_offset = 0
        self.linux_phys_offset = 0
        self.linux_text_offset = 0
        self.linux_mapsize = 0
        self.physmem = {}
        self.physmem["START"] = {}
        self.physmem["END"] = {}
        self.virtmem = {}
        self.virtmem["START"] = {}
        self.virtmem["END"] = {}
        self.virt_regions = 0
        self.phys_regions = 0

class configuration:

    def __init__(self):
        self.arch = None
        self.subarch = None
        self.platform = None
        self.all = []
        self.containers = []

    # Get all name value symbols
    def get_all(self, name, val):
        self.all.append([name, val])

    # Convert line to name value pair, if possible
    def line_to_name_value(self, line):
        parts = line.split()
        if len(parts) > 0:
            if parts[0] == "#define":
                return parts[1], parts[2]
        return None

    # Extract architecture from a name value pair
    def get_arch(self, name, val):
        if name[:len("CONFIG_ARCH_")] == "CONFIG_ARCH_":
            parts = name.split("_", 3)
            self.arch = parts[2].lower()

    # Extract subarch from a name value pair
    def get_subarch(self, name, val):
        if name[:len("CONFIG_SUBARCH_")] == "CONFIG_SUBARCH_":
            parts = name.split("_", 3)
            self.subarch = parts[2].lower()

    # Extract platform from a name value pair
    def get_platform(self, name, val):
        if name[:len("CONFIG_PLATFORM_")] == "CONFIG_PLATFORM_":
            parts = name.split("_", 3)
            self.platform = parts[2].lower()

    # Extract number of containers
    def get_ncontainers(self, name, val):
        if name[:len("CONFIG_CONTAINERS")] == "CONFIG_CONTAINERS":
            self.ncontainers = val

    # TODO: Carry this over to Container() as static method???
    def get_container_parameter(self, id, param, val):
        if param[:len("PAGER_LMA")] == "PAGER_LMA":
            self.containers[id].pager_lma = int(val, 0)
        elif param[:len("PAGER_VMA")] == "PAGER_VMA":
            self.containers[id].pager_vma = int(val, 0)
        elif param[:len("PAGER_SIZE")] == "PAGER_SIZE":
            self.containers[id].pager_size = int(val, 0)
        elif param[:len("LINUX_MAPSIZE")] == "LINUX_MAPSIZE":
            self.containers[id].linux_mapsize = int(val, 0)
        elif param[:len("LINUX_PAGE_OFFSET")] == "LINUX_PAGE_OFFSET":
            self.containers[id].linux_page_offset = int(val, 0)
            self.containers[id].pager_vma += int(val, 0)
        elif param[:len("LINUX_PHYS_OFFSET")] == "LINUX_PHYS_OFFSET":
            self.containers[id].linux_phys_offset = int(val, 0)
            self.containers[id].pager_lma += int(val, 0)
        elif param[:len("LINUX_TEXT_OFFSET")] == "LINUX_TEXT_OFFSET":
            self.containers[id].linux_text_offset = int(val, 0)
            self.containers[id].pager_lma += int(val, 0)
            self.containers[id].pager_vma += int(val, 0)
        elif re.match(r"(VIRT|PHYS){1}([0-9]){1}(_){1}(START|END){1}", param):
            matchobj = re.match(r"(VIRT|PHYS){1}([0-9]){1}(_){1}(START|END){1}", param)
            virtphys, regionidstr, discard1, startend = matchobj.groups()
            regionid = int(regionidstr)
            if virtphys == "VIRT":
                self.containers[id].virtmem[startend][regionid] = val
                if regionid + 1 > self.containers[id].virt_regions:
                    self.containers[id].virt_regions = regionid + 1
            if virtphys == "PHYS":
                self.containers[id].physmem[startend][regionid] = val
                if regionid + 1 > self.containers[id].phys_regions:
                    self.containers[id].phys_regions = regionid + 1
        elif param[:len("OPT_DIRNAME")] == "OPT_DIRNAME":
            dirname = val[1:-1].lower()
            self.containers[id].dirname = dirname
            self.containers[id].name = dirname
        else:
            param1, param2 = param.split("_", 1)
            if param1 == "TYPE":
                if param2 == "LINUX":
                    self.containers[id].type = "linux"
                elif param2 == "C0_POSIX":
                    self.containers[id].type = "cps"
                elif param2 == "BARE":
                    self.containers[id].type = "bare"

    # Extract parameters for containers
    def get_container_parameters(self, name, val):
        matchobj = re.match(r"(CONFIG_CONT){1}([0-9]){1}(\w+)", name)
        if not matchobj:
            return None

        prefix, idstr, param = matchobj.groups()
        id = int(idstr)

        # Create and add new container if this id was not seen
        self.check_add_container(id)

        # Get rid of '_' in front
        param = param[1:]

        # Check and store info on this parameter
        self.get_container_parameter(id, param, val)
        #self.containers_print(self.containers)

    # Used for sorting container members,
    # with this we are sure containers are sorted by id value
    @staticmethod
    def compare_containers(cont, cont2):
        if cont.id < cont2.id:
            return -1
        if cont.id == cont2.id:
            print "compare_containers: Error, containers have same id."
            exit(1)
        if cont.id > cont2.id:
            return 1

    def check_add_container(self, id):
        for cont in self.containers:
            if id == cont.id:
                return

        container = Container(id)
        self.containers.append(container)

        # Make sure elements in order for indexed accessing
        self.containers.sort(self.compare_containers)


    def containers_print(self, containers):
        for c in containers:
            print '\nContainer %d' % c.id
            print 'Container type: %s' % c.type
            print 'Container Name: %s' % c.name
            print 'Container Pager lma: %s' % hex(c.pager_lma)
            print 'Container Pager vma: %s' % hex(c.pager_vma)
            print 'Container Pager size: %s' % hex(c.pager_size)
            print 'Container Virtual regions: %s' % c.virt_regions
            print 'Container Physical regions: %s' % c.phys_regions

    def config_print(self):
        print 'Configuration\n'
        print '-------------\n'
        print 'Arch: %s, %s' % (self.arch, self.subarch)
        print 'Platform: %s' % self.platform
        print 'Symbols:\n %s' % self.all
        print 'Containers: %s' % self.ncontainers
        self.containers_print(self.containers)

def configuration_save(config):
    if not os.path.exists(CONFIG_SHELVE_DIR):
        os.mkdir(CONFIG_SHELVE_DIR)

    config_shelve = shelve.open(CONFIG_SHELVE)
    config_shelve["configuration"] = config

    config_shelve["arch"] = config.arch
    config_shelve["subarch"] = config.subarch
    config_shelve["platform"] = config.platform
    config_shelve["all_symbols"] = config.all
    config_shelve.close()

def configuration_retrieve():
    # Get configuration information
    config_shelve = shelve.open(CONFIG_SHELVE)
    config = config_shelve["configuration"]
    return config