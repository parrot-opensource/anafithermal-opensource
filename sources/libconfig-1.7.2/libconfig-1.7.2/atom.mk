LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := libconfig
LOCAL_DESCRIPTION := Simple library for processing structured configuration files
LOCAL_CATEGORY_PATH := libs

LOCAL_AUTOTOOLS_CONFIGURE_ARGS := --disable-examples

LOCAL_EXPORT_LDLIBS := -lconfig

# The c++ api requires exceptions
ifeq ("$(TARGET_USE_CXX_EXCEPTIONS)","1")
  LOCAL_AUTOTOOLS_CONFIGURE_ARGS += --enable-cxx
  LOCAL_EXPORT_LDLIBS += -lconfig++
else
  LOCAL_AUTOTOOLS_CONFIGURE_ARGS += --disable-cxx
endif

include $(BUILD_AUTOTOOLS)
