# CS4390 Project Test: Run Tracker and Peer Locally

This walkthrough runs both programs on one computer. The tracker runs in Terminal 1. The peer runs in Terminal 2 and sends requests to tracker.

## 1. Download the project

## 2. Set up Ubuntu and GCC

On Windows, use **Ubuntu through WSL** for build and run commands. If Ubuntu is already installed, open it and skip  installation command below.

For WSL, open **PowerShell as Administrator** and run:


`wsl --install -d Ubuntu`


Restart Windows if prompted. Open **Ubuntu** from the Start menu and finish first-time setup. 

In **Ubuntu**, install compiler:


`sudo apt update`

`sudo apt install -y build-essential`

`gcc --version`


If you're already on Linux with GCC installed, use regular terminal.

## 3. Open the project folder in Ubuntu

Replace path in quotes with path for project clone:

`cd "$(wslpath 'C:\Users\YourName\Documents\GitHub\CS4390-Project')`

On native Linux, use `cd` with path where you cloned repository.

## 4. Terminal 1 — Build and start tracker

Run this command from project folder:

`gcc -pthread -o tracker tracker.c worker.c tracker_files.c && ./tracker`

You should see `Listening on port 5000`. Leave this terminal running.

## 5. Terminal 2 — Build and start the peer

Open a **second Ubuntu terminal** and run same `cd` command from step 3. Then run:

`gcc -o peer peer.c && ./peer`

The peer should show `Tracker: 127.0.0.1:5000` and `Enter a tracker request:`. Repository's config files are already set up for this local test.

## 6. Test `createtracker`

At the peer's `Enter a tracker request:` prompt, paste this entire line and press Enter:

`<createtracker hello.txt 5 Demo_file 5d41402abc4b2a76b9719d911017c592 127.0.0.1 6001>`

The values describe the included `Peer01/hello.txt` sample. When the tracker asks for confirmation, type uppercase `Y` and press Enter. Expect:

`Tracker for hello.txt created`

Peer disconnects after success/failure.

If you repeat the create test with the same filename, expect `File Error: hello.txt tracker already exists`. You can still use the existing tracker for the update test.

## 7. Test `updatetracker`

In Terminal 2, start the peer again. Each run handles one request.


`./peer`

At peer prompt, paste this line and press Enter:


`<updatetracker hello.txt 1 4 127.0.0.1 6001>`

Expect `succ`. Can inspect tracker file.

`cat torrents/hello.txt.track`

The peer row for `127.0.0.1` on port `6001` should now show the range `1` to `4`.

To test adding another peer entry, run `./peer` again and paste:


`<updatetracker hello.txt 0 3 127.0.0.1 6002>`

Expect `succ`. Running `cat torrents/hello.txt.track` again should show both port `6001` and port `6002`.

## 8. Stop the tracker or troubleshoot

- **Stop the tracker:** Press `Ctrl+C` in Terminal 1.
- **`peer.c: No such file or directory`:** Run `pwd` and `ls`. Change into the cloned `CS4390-Project` folder and retry.
- **Config file missing:** Start the executable from the folder containing the config files. 
- **`connect: Connection refused`:** Check that Terminal 1 is still running `./tracker` and listening on port `5000`.
- **`Address already in use`:** Check whether you already have a tracker running in another terminal. Use that instance or stop it before starting a new one.
- **Bash syntax error after pasting `<createtracker ...>`:** Run `./peer` first and paste the request at `Enter a tracker request:`.
- **`ferr` on `updatetracker`:** Check that `torrents/hello.txt.track` exists and that the filename in the request is `hello.txt`.