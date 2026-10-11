# Working with PS5 DevLoop

For an agent, or a person, driving a jailbroken PS5 with nobody at the pad. Read
this before the first command; `README.md` says how it works and what was
measured.

## The loop

```bash
tools/ps5loop.py load          # once per console boot (the ELF loader must be up, port 9021)
tools/ps5loop.py open          # the virtual pad; disconnects a real controller (PS reconnects it)
tools/ps5loop.py seq down down cross:300 wait:1000
tools/ps5loop.py grab          # in a title: its own frame
tools/ps5loop.py shot          # anywhere else: the console's screenshot
tools/ps5loop.py close
```

Look, press, look. Never assume a press landed: a menu's default is not always
the first entry (Quake's "Exit game?" starts on NO), and a title takes a moment
after launch before it reads the pad.

## Which picture

| | `grab` | `shot` |
| --- | --- | --- |
| Shows | what the running title drew | the whole screen: home, dialogs, any title |
| Takes | about 1 s | about 3 s |
| Needs | the hook in the title, and a line in `titles.json` | the pad open |
| Leaves on the console | nothing | nothing (the capture is deleted through the system) |

**In a title, use `grab`.** Use `shot` for the home screen and system dialogs, or
for a title that has no hook yet. To learn whether a title is still running, ask
the launcher payload you use (PS5_Vulkan's ps5vkctl: `status`), not a picture.

**When you start work on a title that `titles.json` does not list, give it the
hook first** (below). It is a few lines, and every look at the title afterwards
is three times quicker. Do not settle for `shot` in a title you can build.

## Giving a title grab

The contract, which the payload's `grab REQUEST FOLDER` drives:

1. Once every few frames the title tries to remove a request file in its own
   folder, `/app0/devloop-shot`. Removing it is the check: one system call, which
   fails on every ordinary launch.
2. When the removal succeeds, the title writes a picture of its next frame (JPEG,
   PNG or JXR) as a **new file** in a folder of its own that holds nothing else
   that changes.
3. The payload sends the newest picture in that folder and deletes it.

`/app0` is the title's folder, `/data/homebrew/<TITLE_ID>` from outside. Create
folders 0777 and files 0666, so the payload and FTP can remove them.

Use the engine's own screenshot where it has one. PS5_Quake3e's whole hook
(`ps5/q3/ps5_input.c`), called once a frame:

```c
#define IN_SHOT_REQUEST "/app0/devloop-shot"

static void IN_ShotRequest( void )
{
	static int calls;

	if ( ++calls % 4 == 0 && unlink( IN_SHOT_REQUEST ) == 0 )
		Cbuf_AddText( "screenshotJPEG silent\n" );
}
```

A title with no screenshot of its own reads the frame back in its renderer (the
swapchain image copied to a host-visible buffer, as PS5_VulkanTemplate's
`screenshot` test line does) and writes it with whatever encoder it links.

Then add the title to `titles.json`:

```json
"mytitle": {
  "title_id": "PPSA00000",
  "request": "/data/homebrew/PPSA00000/devloop-shot",
  "folder": "/data/homebrew/PPSA00000/screenshots"
}
```

and prove it: launch the title, `tools/ps5loop.py grab --title mytitle`, look at
the picture, and check the folder is empty afterwards.

## Rules

- **One console job at a time**, and never over a title someone is playing: ask
  the launcher payload first, or look.
- **Do not press Create for less than 1.2 s.** A short press opens the Create
  menu, and no screenshot works until Circle closes it.
- **`open` takes the controller away from the person.** A title reads only its
  user's first controller, so the real one is disconnected. Say so when you
  finish; `open --beside` keeps it, for the home screen only.
- **Leave the console as you found it**: quit the title through its own menu,
  `close` the pad. `shot` and `grab` clean up after themselves; `forget PATH`
  deletes a capture properly (file, thumbnail and database row) if one is left.
- **Never edit `/system_data/priv/mms/av_content.db`** or delete files under
  `/user/av_contents` by hand: the gallery's database and its files go out of
  step. `forget` asks the system to do it.
- Never print the console's address: it lives in `PS5_HOST` or the ignored `.env`.
