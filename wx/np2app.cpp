/* === wxApp class for NP2kai wx port === */

#include <compiler.h>
#include "np2app.h"
#include "np2frame.h"
#include "np2.h"
#include "ini.h"

#include <wx/filename.h>

#if defined(__WXOSX__)
#include "macos/macview.h"
#endif

wxIMPLEMENT_APP(Np2App);

bool Np2App::OnInit()
{
	m_initialized = false;
	m_launching = true;

	/* wxWidgets image handlers (for PNG icons etc.) */
	wxInitAllImageHandlers();

#if defined(__WXOSX__)
	/* move these items into the application menu */
	s_macAboutMenuItemId       = ID_OTHER_ABOUT;
	s_macPreferencesMenuItemId = ID_EMU_PREFERENCE;
	s_macExitMenuItemId        = ID_EMU_EXIT;
#endif

	/* SDL: init audio only (no video - that's wxWidgets' job) */
	if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
		wxMessageBox(wxString::Format("SDL_Init failed: %s", SDL_GetError()),
		             "Error", wxOK | wxICON_ERROR);
		return false;
	}

	/* Parse command-line options */
	if (!ParseArgs()) return false;

	/* Initialize emulator core */
	wxString argv0 = argv[0];
	if (np2_initialize(argv0.ToUTF8().data()) != SUCCESS) {
		wxMessageBox("Failed to initialize emulator core.", "Error",
		             wxOK | wxICON_ERROR);
		SDL_Quit();
		return false;
	}
	m_initialized = true;

	/* Determine initial window size from the saved Screen Size */
	if (np2oscfg.scrnmul < 1 || np2oscfg.scrnmul > 3) np2oscfg.scrnmul = 1;
	int w = FULLSCREEN_WIDTH  * np2oscfg.scrnmul;
	int h = FULLSCREEN_HEIGHT * np2oscfg.scrnmul;

	wxPoint pos(wxDefaultPosition);
	if (np2oscfg.winx >= 0 && np2oscfg.winy >= 0)
		pos = wxPoint(np2oscfg.winx, np2oscfg.winy);

#if defined(CPUCORE_IA32)
	wxString title = "wx NP21kai (IA-32)";
#else
	wxString title = "wx NP2kai (286)";
#endif

	Np2Frame *frame = new Np2Frame(title, pos, wxSize(w, h));
	/* w/h are client-area dimensions; adjust frame size accordingly */
	frame->SetClientSize(w, h);
	frame->Show(true);
	SetTopWindow(frame);
#if defined(__WXOSX__)
	/* a bare executable started from a shell stays in the background */
	macview_activateapp();
#endif
	/* files Finder hands over at launch arrive before this runs */
	CallAfter([this]() { m_launching = false; });

	return true;
}

int Np2App::OnExit()
{
	/* np2_terminate() is called from Np2Frame::OnClose */
	SDL_Quit();
	return wxApp::OnExit();
}

bool Np2App::ParseArgs(void)
{
	for (int i = 1; i < argc; i++) {
		wxString arg = argv[i];
		if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
			/* Override config path via environment variable picked up by initgetfile */
			wxString next = argv[++i];
			wxSetEnv("NP2KAI_CONFIG", next);
		} else if (arg == "-h" || arg == "--help") {
			wxMessageBox(
			    wxString::Format(
			        "Usage: %s [options] [disk images...]\n"
			        "  -c <file>   Config file path\n"
			        "  -h          Help\n",
			        argv[0]),
			    "NP2kai wx", wxOK | wxICON_INFORMATION);
			return false;
		} else if (arg.StartsWith("-")) {
			/* ignore unknown options (e.g. macOS -psn_...) */
		} else {
			wxFileName fn(arg);
			fn.MakeAbsolute();
			if (fn.FileExists()) {
				m_startFiles.Add(fn.GetFullPath());
				np2_addmedia(fn.GetFullPath().ToUTF8().data());
			}
		}
	}
	return true;
}

#if defined(__WXOSX__)
/* Finder "Open With", drop on the Dock icon, `open -a`.
 * Cocoa also replays command-line file arguments here at launch;
 * those were already applied by np2_initialize(), so skip them once. */
void Np2App::MacOpenFiles(const wxArrayString &fileNames)
{
	Np2Frame *frame = wxDynamicCast(GetTopWindow(), Np2Frame);
	wxArrayString files;
	for (const wxString &name : fileNames) {
		int idx = m_startFiles.Index(name);
		if (idx != wxNOT_FOUND) {
			m_startFiles.RemoveAt(idx);
			continue;
		}
		files.Add(name);
	}
	if (!frame || files.IsEmpty()) return;

	if (m_launching) {
		/* app opened by double-clicking an image: boot from it */
		frame->BootMedia(files);
	} else {
		for (const wxString &name : files) frame->OpenMediaFile(name);
	}
}
#endif
