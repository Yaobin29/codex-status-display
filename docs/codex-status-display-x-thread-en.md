# X / Twitter Thread Draft

Attach video: `assets/codex-status-display-demo.mp4`

1/ I built a tiny ESP32 screen that shows my Codex chat status on my desk.

Not tasks. Not a dashboard. Just the two things I actually need while agents are running in the background:

RUNNING and DONE TO READ.

2/ The first version was too broad.

It read task lists, automations, review queues, and other local state. That produced confusing numbers like “approve 92”.

The lesson: the screen should track Codex chats, not every possible workflow artifact.

3/ The final mental model is much smaller:

- Running = Codex chats still active
- Done = recently completed chats I have not read yet
- Response needed = a running chat waiting for my input

Response-needed is shown inside the running list, not as a separate category.

4/ Hardware:

- ESP32 Dev Module
- ST7789 display
- 135 x 240
- Arduino firmware
- USB serial for debug
- WiFi for daily use

I reused pin settings from an older weather-screen sketch.

5/ The host bridge is a small Python service.

It reads stable local Codex state, normalizes it, and exposes a compact JSON payload under 1 KB.

The ESP32 polls:

`http://<mac-lan-ip>:8787/wire`

every 10 seconds.

6/ The UI went through a few iterations.

The useful version is simple:

- big Running number
- big Done unread number
- short chat list
- green rows for normal running
- orange/red rows when I need to respond
- tiny pixel animation so I know it is alive

7/ I like this because it turns agent work into ambient information.

I do not need to keep checking the Codex window. The screen tells me whether the system is still working, finished, or waiting for me.

8/ Small displays force good product thinking.

The breakthrough was not adding more data. It was deleting the wrong data until the screen only showed what changes my next action.

#Codex #ESP32 #Arduino #AIagents #maker
