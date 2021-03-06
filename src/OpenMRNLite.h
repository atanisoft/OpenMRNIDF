/** \copyright
 * Copyright (c) 2018, Balazs Racz
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are  permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * \file OpenMRNLite.h
 *
 * Main include file for the OpenMRN library to be used in an Arduino
 * compilation environment.
 *
 * @author Balazs Racz
 * @date 24 July 2018
 */

#ifndef _ARDUINO_OPENMRNLITE_H_
#define _ARDUINO_OPENMRNLITE_H_

#include <Arduino.h>

#include "openmrn_features.h"

#include "CDIHelper.hxx"
#include "freertos_drivers/arduino/Can.hxx"
#include "freertos_drivers/arduino/WifiDefs.hxx"
#include "openlcb/SimpleStack.hxx"
#include "utils/constants.hxx"
#include "utils/FileUtils.hxx"
#include "utils/GridConnectHub.hxx"
#include "utils/logging.h"
#include "utils/Uninitialized.hxx"

#if defined(ESP32)

#include <esp_task.h>
#include <esp_task_wdt.h>

// Include the ESP-IDF based GPIO support.
#include "freertos_drivers/esp32/Esp32Gpio.hxx"
#include "freertos_drivers/esp32/Esp32HardwareCanAdapter.hxx"
#include "freertos_drivers/esp32/Esp32HardwareSerialAdapter.hxx"
#include "freertos_drivers/esp32/Esp32HardwareTwai.hxx"
#include "freertos_drivers/esp32/Esp32WiFiManager.hxx"

// On the ESP32 we have persistent file system access so enable
// dynamic CDI.xml generation support
#define HAVE_FILESYSTEM 1

#else

// Include generic Arduino API compatible GPIO pin support.
#include "freertos_drivers/arduino/ArduinoGpio.hxx"
#endif // ESP32

#ifdef ARDUINO_ARCH_STM32

#include "freertos_drivers/stm32/Stm32Can.hxx"

#endif

namespace openmrn_arduino
{

/// Bridge class that connects an Arduino API style serial port (sending CAN
/// frames via gridconnect format) to the OpenMRN core stack. This can be
/// generally used for USB ports or TCP sockets.
template <class SerialType> class SerialBridge : public Executable
{
public:
    /// Constructor.
    ///
    /// @param port is the Arduino serial implementation (usually &Serial).
    /// @param can_hub is the core CAN frame router of the OpenMRN stack,
    /// usually comes from stack()->can_hub().
    SerialBridge(SerialType *port, CanHubFlow *can_hub)
        : service_(can_hub->service())
        , port_(port)
    {
        GCAdapterBase::CreateGridConnectAdapter(&txtHub_, can_hub, false);
        txtHub_.register_port(&writePort_);
    }

    /// @todo (balazs.racz): add destructor and tear down the link without
    /// memory leaks.

    /// Called by the loop.
    void run() override
    {
        loop_for_write();
        loop_for_read();
    }

private:
    /// Handles data going out of OpenMRN and towards the serial port.
    void loop_for_write()
    {
        if (!writeBuffer_)
        {
            return;
        }
        size_t len = port_->availableForWrite();
        if (!len)
        {
            return;
        }
        size_t to_write = writeBuffer_->data()->size() - writeOfs_;
        if (len > to_write)
            len = to_write;
        port_->write((const uint8_t*)writeBuffer_->data()->data() + writeOfs_, len);
        writeOfs_ += len;
        if (writeOfs_ >= writeBuffer_->data()->size())
        {
            writeBuffer_ = nullptr;
            // wakes up state flow to release buffer and take next from the
            // queue.
            writePort_.notify();
        }
    }

    /// Handles data coming in from the serial port and sends it to OpenMRN.
    void loop_for_read()
    {
        int av = port_->available();
        if (av <= 0)
        {
            return;
        }
        auto *b = txtHub_.alloc();
        b->data()->skipMember_ = &writePort_;
        b->data()->resize(av);
        port_->readBytes((char*)b->data()->data(), b->data()->size());
        txtHub_.send(b);
    }

    friend class WritePort;

    /// Finite state machine running in the OpenMRN cooperative executor,
    /// registered to receive data that needs to be sent to the port. It will
    /// keep the pointer to the output buffer while the loop processes the data
    /// in it.
    class WritePort : public HubPort
    {
    public:
        /// Constructor.
        ///
        /// @param parent is the bridge object that owns *this.
        /// @param service holds the cooperative executor coming from the
        /// OpenMRN stack. Any service from OpenMRN will do.
        WritePort(SerialBridge *parent, Service *service)
            : HubPort(service)
            , parent_(parent)
        {
        }

        /// State machine state when a new buffer of data to be sent shows
        /// up. Must not block.
        Action entry() override
        {
            parent_->writeBuffer_ = message();
            parent_->writeOfs_ = 0;
            // Pauses the state machine until an external event calls notify().
            return wait_and_call(STATE(write_done));
        }

        /// We get to this state when the output buffer's data has fully been
        /// sent to the hardware and the notofy() call was made. It releases
        /// the memory and continues processing any further data piled up in
        /// the input queue.
        Action write_done()
        {
            return release_and_exit();
        }

    private:
        /// Parent that owns *this.
        SerialBridge *parent_;
    };

    /// Access to the stack's executor.
    Service *service_;
    /// Instance of the state machine.
    WritePort writePort_{this, service_};
    /// Arduino device instance.
    SerialType *port_;
    /// Buffer we are writing the output from right now. These bytes go from
    /// OpenMRN to the device.
    Buffer<HubData> *writeBuffer_{nullptr};
    /// Offset in the output buffer of the next byte to write.
    size_t writeOfs_;
    /// Hub for the textual data.
    HubFlow txtHub_{service_};
};

/// Bridge class that connects a native CAN controller to the OpenMRN core
/// stack, sending and receiving CAN frames directly. The CAN controller must
/// have a driver matching the Can controller base class defined in
/// OpenMRN/arduino.
class CanBridge : public Executable
{
public:
    /// Constructor.
    ///
    /// @param port is the CAN hardware driver implementation.
    /// @param can_hub is the core CAN frame router of the OpenMRN stack,
    /// usually comes from stack()->can_hub().
    CanBridge(Can *port, CanHubFlow *can_hub)
        : port_(port)
        , canHub_(can_hub)
    {
        port_->enable();
        can_hub->register_port(&writePort_);
    }

    ~CanBridge()
    {
        port_->disable();
    }

    /// Called by the loop.
    void run() override
    {
        loop_for_write();
        loop_for_read();
    }

private:
    /// Handles data going out of OpenMRN and towards the CAN port.
    void loop_for_write()
    {
        if (!writeBuffer_)
        {
            return;
        }
        if (port_->availableForWrite() <= 0)
        {
            return;
        }
        port_->write(writeBuffer_->data());
        writeBuffer_ = nullptr;
        writePort_.notify();
    }

    /// Handles data coming from the CAN port.
    void loop_for_read()
    {
        while (port_->available())
        {
            auto *b = canHub_->alloc();
            port_->read(b->data());
            b->data()->skipMember_ = &writePort_;
            canHub_->send(b);
        }
    }

    friend class WritePort;
    class WritePort : public CanHubPort
    {
    public:
        WritePort(CanBridge *parent)
            : CanHubPort(parent->canHub_->service())
            , parent_(parent)
        {
        }

        Action entry() override
        {
            parent_->writeBuffer_ = message();
            return wait_and_call(STATE(write_done));
        }

        Action write_done()
        {
            return release_and_exit();
        }

    private:
        CanBridge *parent_;
    };

    /// Hardware driver.
    Can *port_;
    /// Next buffer we are trying to write into the driver's FIFO.
    Buffer<CanHubData> *writeBuffer_{nullptr};
    /// Connection to the stack.
    CanHubFlow *canHub_;
    /// State flow with queues for output frames generated by the stack.
    WritePort writePort_{this};
};

/// Main class to declare the OpenMRN stack. Create one instance of this in the
/// root file of your sketch. Prefer to supply the Node ID during construction.
class OpenMRN : private Executable
{
public:
    /// Constructor if the Node ID is not known. Must call init(...) with the
    /// Node ID beofre using anything related to the stack, including
    /// instantiating objects that depend on the stack. Prefer the other
    /// constructor.
    OpenMRN()
    {
    }

    /// Use this constructor if stack() needs to be accessed during the time of
    /// the static construction.
    OpenMRN(openlcb::NodeID node_id);

    /// Call this function once if the empty constructor was used.
    void init(openlcb::NodeID node_id)
    {
        stack_.emplace(node_id);
    }

    /// @return pointer to the OpenMRN stack. Do not call before init().
    openlcb::SimpleCanStack *stack()
    {
        return stack_.operator->();
    }

    /// Call this function from the setup() function of the Arduino sketch
    /// after all other Arduino subsystems or libraries have been initialized.
    void begin()
    {
        stack_->start_stack(false);
    }

    /// Call this function from the loop() function of the Arduino sketch.
    void loop()
    {
        for (auto *e : loopMembers_)
        {
#if defined(ESP32) && CONFIG_TASK_WDT
            // Feed the watchdog so it doesn't reset the ESP32
            esp_task_wdt_reset();
#endif // ESP32 && CONFIG_TASK_WDT
            e->run();
        }
    }

// For single threaded platforms (including ESP32-S2) do not expose the support
// for spawning a seperate thread for the OpenMRN executor.
#if !defined(OPENMRN_FEATURE_SINGLE_THREADED) && \
    !defined(CONFIG_IDF_TARGET_ESP32S2)
    /// Entry point for the executor thread when @ref start_executor_thread is
    /// called with donate_current_thread set to false.
    static void thread_entry(void *arg)
    {
        OpenMRN *p = (OpenMRN *)arg;
        p->loop_executor();
    }

    /// Donates the calling thread to the @ref Executor.
    ///
    /// Note: this method will not return until the @ref Executor has shutdown.
    void loop_executor()
    {
#if defined(ESP32) && CONFIG_TASK_WDT
        uint32_t current_core = xPortGetCoreID();
        TaskHandle_t idleTask = xTaskGetIdleTaskHandleForCPU(current_core);
        // check if watchdog is enabled and print a warning if it is
        if (esp_task_wdt_status(idleTask) == ESP_OK)
        {
            LOG(WARNING, "WDT detected as enabled on core %d!", current_core);
        }
#endif // ESP32 && CONFIG_TASK_WDT
        haveExecutorThread_ = true;

        // donate this thread to the executor
        stack_->executor()->thread_body();
    }

    /// Starts a thread for the @ref Executor used by OpenMRN.
    ///
    /// Note: On the ESP32 the watchdog timer is disabled for the PRO_CPU prior
    /// to starting the background task for the @ref Executor.
    void start_executor_thread()
    {
        haveExecutorThread_ = true;
// On the ESP32 create a task on the PRO_CPU (core 0) which is typically not in
// active use by the arduino-esp32 stack. The arduino-esp32 "app_main" starts
// executing on the PRO_CPU but creates a "loopTask" which runs from the
// APP_CPU (core 1), this loopTask is responsible for executing setup() and
// loop() from the user code.
//
// For the non-ESP32 platforms this will invoke the Executor::start_thread()
// method.
#ifdef ESP32
#if CONFIG_TASK_WDT_CHECK_IDLE_TASK_CPU0
        // Remove IDLE0 task watchdog, because the openmrn task sometimes
        // uses 100% cpu and it is pinned to CPU 0.
        disableCore0WDT();
#endif // CONFIG_TASK_WDT_CHECK_IDLE_TASK_CPU0
        xTaskCreatePinnedToCore(
            thread_entry, "OpenMRN", config_arduino_openmrn_stack_size(), this,
            config_arduino_openmrn_task_priority(), nullptr, PRO_CPU_NUM);
#else
        stack_->executor()->start_thread(
            "OpenMRN", config_arduino_openmrn_task_priority(),
            config_arduino_openmrn_stack_size());
#endif // ESP32
    }
#endif // !OPENMRN_FEATURE_SINGLE_THREADED && !CONFIG_IDF_TARGET_ESP32S2

    /// Adds a serial port to the stack speaking the gridconnect protocol, for
    /// example to do a USB connection to a computer. This is the protocol that
    /// USB-CAN adapters for LCC are speaking to the computer.
    ///
    /// Example:
    /// void setup() {
    ///   ...
    ///   openmrn.begin();
    ///   openmrn.add_gridconnect_port(&Serial);
    ///   ...
    /// }
    ///
    /// @param port is the serial port instance from Arduino.
    template <class SerialType> void add_gridconnect_port(SerialType *port)
    {
        loopMembers_.push_back(
            new SerialBridge<SerialType>(port, stack()->can_hub()));
    }

#ifdef OPENMRN_FEATURE_EXECUTOR_SELECT
    /// Adds a hardware CAN port to the stack with select-based asynchronous
    /// driver API.
    ///
    /// Example (ESP32):
    /// Esp32Twai twai("/dev/twai", GPIO_NUM_5, GPIO_NUM_4);
    /// void setup() {
    ///   ...
    ///   twai.hw_init();
    ///   openmrn.begin();
    ///   openmrn.add_can_port_select("/dev/twai/twai0");
    /// }
    /// @param device path to open for the CAN device.
    void add_can_port_select(const char *device)
    {
        stack()->add_can_port_select(device);
    }
#endif // OPENMRN_FEATURE_EXECUTOR_SELECT

    /// Adds a hardware CAN port to the stack. If multiple ports are added,
    /// OpenMRN will be forwarding traffic frames between them: the simplest
    /// CAN-USB sketch just adds the serial port connecting to the computer and
    /// the hardware CAN port.
    void add_can_port(Can *port)
    {
        loopMembers_.push_back(new CanBridge(port, stack()->can_hub()));
    }

#if defined(HAVE_FILESYSTEM)
    /// Creates the XML representation of the configuration structure and saves
    /// it to a file on the filesystem. Must be called after SPIFFS.begin() but
    /// before calling the {\link create_config_file_if_needed} method. The
    /// config file will be re-written whenever there was a change in the
    /// contents. It is also necessary to declare the static compiled-in CDI to
    /// be empty:
    /// ```
    ///    namespace openlcb {
    ///    // This will stop openlcb from exporting the CDI memory space
    ///    // upon start.
    ///    extern const char CDI_DATA[] = "";
    ///    }  // namespace openlcb
    /// ```
    /// @param cfg is the global configuration instance (usually called cfg).
    /// @param filename is where the xml file can be stored on the
    /// filesystem. For example "/spiffs/cdi.xml".
    /// @returns true if the cdi.xml was updated, false otherwise.
    template <class ConfigDef>
    bool create_config_descriptor_xml(
        const ConfigDef &config, const char *filename)
    {
        return CDIHelper::create_config_descriptor_xml(
            config, filename, stack());
    }
#endif // HAVE_FILESYSTEM

private:
    /// Callback from the loop() method. Internally called.
    void run() override
    {
// If we are running on a platform where we can have a dedicated executor
// thread, check if it has been created and exit early if it has.
#if !defined(OPENMRN_FEATURE_SINGLE_THREADED) && \
    !defined(CONFIG_IDF_TARGET_ESP32S2)
        if (haveExecutorThread_)
        {
            return;
        }
#endif
        stack_->executor()->loop_some();
    }

    /// Storage space for the OpenLCB stack. Will be constructed in init().
    uninitialized<openlcb::SimpleCanStack> stack_;

    /// List of objects we need to call in each loop iteration.
    vector<Executable *> loopMembers_{{this}};

#if !defined(OPENMRN_FEATURE_SINGLE_THREADED) && \
    !defined(CONFIG_IDF_TARGET_ESP32S2)
    /// True if there is a separate thread running the executor.
    bool haveExecutorThread_{false};
#endif // !OPENMRN_FEATURE_SINGLE_THREADED && !CONFIG_IDF_TARGET_ESP32S2
};

} // namespace openmrn_arduino

using openmrn_arduino::OpenMRN;

#endif // _ARDUINO_OPENMRNLITE_H_
