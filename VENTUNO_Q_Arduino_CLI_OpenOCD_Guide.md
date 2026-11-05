# VENTUNO Q — Compile, Upload, Stop, and Restart an MCU Sketch from Linux

**Practical guide — experimentally verified on an Arduino VENTUNO Q**  
**Tested environment:** `arduino:zephyr` core **1.1.0**, Arduino App CLI **0.14.0**, Arduino CLI, OpenOCD, and `gpiod` tools.  
**Test application:** GPS receiver → MCU UART → `BridgeTCPClient<>` → MQTT broker → IoT MQTT Panel.

> **Scope:** The commands below worked on the tested VENTUNO Q. Paths, the IP address, and the GPIO numbering reflect that setup. Direct GPIO and OpenOCD operations reproduce mechanisms used by Arduino tooling, but should not be assumed to constitute a separately supported public API. Hardware mappings and loader behavior may change with software revisions.

## 1. Purpose and findings

The goal was to run a GPS/MQTT sketch on the VENTUNO Q **MCU without launching an Arduino App Lab application**, then **interrupt and resume its execution without replacing it with an empty sketch**.

Tests confirmed:

- Arduino CLI can compile and upload the sketch over the network without an App Lab application running.
- The MCU sketch can publish MQTT messages using `Arduino_RouterBridge.h` and `BridgeTCPClient<>`, with network transport provided by the **Linux MPU**.
- With `wait_linux_boot=yes`, the sketch resumes automatically after a full board reboot.
- With `wait_linux_boot=app`, the sketch starts immediately after upload; resetting the MCU leaves the Zephyr loader waiting; an OpenOCD memory write subsequently starts the sketch again **without another upload**.

**Project repository:** <https://github.com/philippe86220/VENTUNO-Q-GPS-MQTT>  
**Arduino forum discussion and explanations by @ptillisch:** <https://forum.arduino.cc/t/ventuno-q-running-a-persistent-gps-mqtt-sketch-using-arduino-cli-without-app-lab/1466507>

## 2. Test architecture

```text
GPS receiver
    | UART / NMEA sentences
    v
VENTUNO Q MCU (Arduino / Zephyr sketch)
    | Arduino_RouterBridge.h + BridgeTCPClient<>
    v
VENTUNO Q MPU (Linux networking services)
    | TCP, port 1883
    v
MQTT broker: broker.emqx.io
    |
    v
IoT MQTT Panel (smartphone)
```

**Important:** “Without App Lab” does **not** mean “without Linux.” The sketch still relies on the MPU-side networking infrastructure to reach the broker. For MQTT packet construction and GPS processing, see the repository's main `README.md`.

## 3. Identify the board and available startup modes

List detected boards:

```bash
arduino-cli board list
```

Example observed during testing (the address is specific to the local network):

```text
Port          Protocol  Type          Board Name         FQBN
192.168.1.17  network   Network Port  Arduino Ventuno Q  arduino:zephyr:ventunoq
```

Inspect the board options:

```bash
arduino-cli board details -b arduino:zephyr:ventunoq
```

Relevant startup options:

| Option | Practical meaning | Test status |
|---|---|---|
| `wait_linux_boot=yes` | Wait for Linux to boot before running the sketch. | Verified |
| `wait_linux_boot=no` | Start without waiting for Linux. | **Not tested here** |
| `wait_linux_boot=app` | Wait for a startup authorization flag in the MCU backup SRAM. | Verified |

The `wait_linux_boot=app` setting **does not inherently require the App Lab graphical interface**. The startup authorization can be supplied directly through OpenOCD.

## 4. Standalone operation and persistence with `wait_linux_boot=yes`

A compile-only check can be performed without changing the sketch installed on the board:

```bash
arduino-cli compile \
  --fqbn arduino:zephyr:ventunoq:wait_linux_boot=yes \
  ~/ArduinoApps/ventuno-q-gps-mqtt-main/sketch/sketch.ino
```

This preliminary check is optional: `arduino-cli compile --upload` compiles **and then uploads** in one command.

Command successfully used:

```bash
arduino-cli compile --upload \
  --fqbn arduino:zephyr:ventunoq:wait_linux_boot=yes \
  --port 192.168.1.17 \
  ~/ArduinoApps/ventuno-q-gps-mqtt-main/sketch/sketch.ino
```

**Observed result:** The GPS/MQTT sketch published data without an App Lab application running. Following a complete VENTUNO Q reboot, MQTT publications resumed automatically.

### Network upload authentication: an unresolved observation

Some network uploads prompted:

```text
Uploading to specified board using network protocol requires the following info:
Password:
```

The first upload did not prompt for a password. Later tests showed intermittent prompts, sometimes absent after opening an application in App Lab and reappearing after some time. **The cause was not established.** These observations do not establish a particular authentication mechanism, token lifetime, or security issue.

## 5. Earlier workaround: replace the running sketch with an empty one

Before investigating the loader, two approaches were tested:

1. Open an App Lab application containing an empty sketch.
2. Upload an empty sketch using Arduino CLI.

The empty sketch was stored at `~/ArduinoApps/vide/sketch/sketch.ino`:

```cpp
void setup() {
  // put your setup code here, to run once:
}

void loop() {
  // put your main code here, to run repeatedly:
}
```

Replacement command:

```bash
arduino-cli compile --upload \
  --fqbn arduino:zephyr:ventunoq:wait_linux_boot=yes \
  --port 192.168.1.17 \
  ~/ArduinoApps/vide/sketch/sketch.ino
```

**Limitation:** This **replaces** the installed sketch. It is not a stop command that preserves the previous sketch for a later restart.

## 6. How “Wait for App” works: explanation from @ptillisch

After examining the Arduino App CLI orchestrator and Zephyr loader sources, @ptillisch explained that stopping an App Lab application does not perform a normal software shutdown of the MCU sketch: the **MCU is reset**. When the sketch was built with `wait_linux_boot=app`, the restarted loader waits for authorization instead of immediately launching it.

An early implementation idea involved signaling through a GPIO between the MPU and MCU. The implemented mechanism instead uses the onboard debug probe and **OpenOCD to write a flag into the MCU's backup SRAM**.

| Item | Role |
|---|---|
| `0x40036400` | Startup flag address in MCU backup SRAM. |
| `0xCAFFEEEE` | Startup flag value written by the Arduino upload script. |
| `mww` | OpenOCD **32-bit word** memory write. |
| `mwb` | OpenOCD **8-bit byte** memory write. |

The loader in the tested core version accepts **any nonzero value**. However, @ptillisch recommends:

```text
mww 0x40036400 0xCAFFEEEE
```

This matches the actual Arduino upload script. In an earlier reply, he accidentally suggested `mwb` after adapting an old command from a commit message; he subsequently clarified the difference and corrected his recommendation. Using the canonical 32-bit write avoids relying on the loader's currently permissive nonzero check, although it does **not** guarantee future compatibility.

## 7. Verified procedure: upload, interrupt, and resume

### Step A — Compile, upload, and start the GPS/MQTT sketch in “Wait for App” mode

```bash
arduino-cli compile --upload \
  --fqbn arduino:zephyr:ventunoq:wait_linux_boot=app \
  --port 192.168.1.17 \
  ~/ArduinoApps/ventuno-q-gps-mqtt-main/sketch/sketch.ino
```

**Verified result:** The sketch starts **immediately after upload**, and GPS data reaches the MQTT broker. This is expected: the upload script itself writes the startup flag. “Wait for App” therefore does not prevent the sketch from running immediately after a successful upload.

### Step B — Reset the MCU to interrupt sketch execution

```bash
gpioset gpiochip2 78=0 && gpioget gpiochip2 78
```

On the tested board, **MPU GPIO 78** on `gpiochip2` is connected to the MCU **RESET** line. The first command drives the line low; in the tested environment, the second accesses it as an input and releases the reset line, as described by @ptillisch.

**Verified result:** MQTT publications stop. The MCU has been reset, and the Zephyr loader waits for a new startup authorization instead of executing the installed sketch. **The sketch remains installed.**

**Caution:** This directly manipulates a hardware reset line. Its behavior depends on the `gpiochip2`/GPIO 78 mapping and the installed `gpiod` implementation. Do not blindly reuse it on other boards or software configurations. Reset is abrupt: it does not guarantee an MQTT `DISCONNECT` packet or application cleanup.

### Step C — Authorize sketch execution again without uploading

```bash
/opt/openocd/bin/openocd \
  --search /opt/openocd \
  --search /opt/openocd/share/openocd/scripts \
  --file openocd_gpiod.cfg \
  --command "init; mww 0x40036400 0xCAFFEEEE; shutdown"
```

OpenOCD writes the startup flag expected by the Zephyr loader.

**Verified result:** The GPS/MQTT sketch resumes execution and MQTT publications reappear, **without recompiling or uploading the sketch**.

### State sequence

```text
Upload with wait_linux_boot=app
    +-- Upload script writes startup flag --> Sketch running --> MQTT active
                                               |
                           gpioset + gpioget   | MCU RESET
                                               v
                                      Zephyr loader waiting
                                               |
                           OpenOCD + mww       | Startup flag
                                               v
                                         Sketch running --> MQTT active
```

**Important:** Step B does not power off the MCU. The MCU runs its loader, which waits for the startup authorization. This is **not** a graceful application shutdown.

## 8. Verified results and limitations

| Observation | Status |
|---|---|
| GPS/MQTT sketch compilation using Arduino CLI | **Verified** |
| Network upload without an App Lab application running | **Verified** |
| MQTT publications using `BridgeTCPClient<>` and Linux MPU networking | **Verified** |
| Automatic restart after a full board reboot with `wait_linux_boot=yes` | **Verified** |
| Immediate start after upload with `wait_linux_boot=app` | **Verified** |
| Interrupt publications by resetting the MCU via GPIO 78 | **Verified** |
| Resume execution using OpenOCD `mww`, without re-uploading | **Verified** |
| Graceful MQTT shutdown (`DISCONNECT`) before reset | **Not tested** |
| `wait_linux_boot=no` behavior | **Not tested** |
| Definitive explanation of intermittent network authentication prompts | **Unresolved** |
| Compatibility with all future Zephyr core versions | **Not guaranteed** |

## 9. Technical references

- [Arduino forum: discussion and test confirmation](https://forum.arduino.cc/t/ventuno-q-running-a-persistent-gps-mqtt-sketch-using-arduino-cli-without-app-lab/1466507)
- [Arduino App CLI 0.14.0: application stop handling](https://github.com/arduino/arduino-app-cli/blob/v0.14.0/internal/orchestrator/orchestrator.go#L330-L336)
- [ArduinoCore-zephyr 1.1.0: loader startup flag check](https://github.com/arduino/ArduinoCore-zephyr/blob/1.1.0/loader/main.c#L282-L286)
- [VENTUNO Q upload script: canonical flag write](https://github.com/arduino/ArduinoCore-zephyr/blob/1.1.0/variants/arduino_ventuno_q_stm32h5f5xx/flash_sketch.cfg#L29)
- [Earlier development of the memory-based startup signal](https://github.com/arduino/ArduinoCore-zephyr/commit/3f4f5fe63936e2ccf64e33a00d7a1b3465a251ac)
- [Commit message containing the earlier `mwb` example](https://github.com/arduino/ArduinoCore-zephyr/commit/164a4d9160dabb9c4d317b4b3b6501a12429e698)
- [OpenOCD general commands: `mww`](https://openocd.org/doc/html/General-Commands.html#index-mww)
- [STM32H5 backup SRAM definition in the Zephyr device tree](https://github.com/arduino/zephyr/blob/zephyr-arduino-v4.4.2/dts/arm/st/h5/stm32h5.dtsi#L181-L181)
- [Zephyr backup SRAM documentation](https://docs.zephyrproject.org/latest/samples/boards/st/backup_sram/README.html)

## 10. Conclusion

These experiments demonstrate that `wait_linux_boot=app` does **not** require the Arduino App Lab interface. It relies on a **Zephyr loader startup authorization mechanism** that can be controlled from Linux using tools available on the tested VENTUNO Q.

In this configuration, the GPS/MQTT sketch can be **uploaded once**, then **interrupted and started again** without replacement or recompilation, by resetting the MCU and subsequently setting its startup flag.

## Acknowledgements

Special thanks to **@ptillisch** for investigating the Arduino and Zephyr source code, explaining the loader mechanism in detail, and correcting the OpenOCD command; and to the Arduino community for the technical discussion.

The experiments were carried out and verified on a VENTUNO Q by **Philippe Costes**. This documentation was prepared with the assistance of **OpenAI's ChatGPT (GPT-6)**.
