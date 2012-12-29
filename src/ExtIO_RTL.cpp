/*
 * ExtIO wrapper for librtlsdr
 * Copyleft by José Araújo [josemariaaraujo@gmail.com]
 * Don't care about licenses (DWTFYW), but per GNU I think I'm required to leave the following here:
 *
 * Based on original work from Ian Gilmour (MM6DOS) and Youssef Touil from SDRSharp
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* ******************************************************************************
 * Simple ExtIO wrapper for librtlsdr.  
 * 
 * These guys done a great job on the lib. All credits to them
 *
 * Original librtlsdr git:
 *
 * git://git.osmocom.org/rtl-sdr
 */


#include <Windows.h>
#include <WindowsX.h>
#include <commctrl.h>
#include <process.h>
#include <tchar.h>
#include <new>
#include <stdio.h>

#include "resource.h"
#include "rtl-sdr.h"
#include "ExtIO_RTL.h"

#define EXTIO_HWTYPE_16B	3

#define MAX_PPM	1000
#define MIN_PPM	-1000
static int ppm_default=0;

typedef struct sr {
	double value;
	TCHAR *name;
} sr_t;

static sr_t samplerates[] = {
	{  960000.0, TEXT("0.96 Msps") },
	{ 1028571.0, TEXT("1.02 Msps") },
	{ 1200000.0, TEXT("1.2 Msps") },
	{ 1440000.0, TEXT("1.44 Msps") },
	{ 1800000.0, TEXT("1.8 Msps") },
	{ 2400000.0, TEXT("2.4 Msps") },
	{ 2880000.0, TEXT("2.88 Msps")},
	{ 3200000.0, TEXT("3.2 Msps") }
};

static int samplerate_default=5; // 2.4 Msps
#define MAXRATE		3200000 
#define MINRATE		900001 

static int TunerAGC_default=1;
static int RTLAGC_default=0;
static int gain_default=0;

static int buffer_sizes[] = { //in kBytes
	1,
	2,
	4,
	8,
	16,
	32,
	64,
	128,
	256,
	512,
	1024
};

static int buffer_default=6;// 64kBytes

static int buffer_len;

typedef struct {
	char vendor[256], product[256], serial[256];
} device;

static device *connected_devices = NULL;

static rtlsdr_dev_t *dev = NULL;
static int device_count = 0;

// Thread handle
HANDLE worker_handle=INVALID_HANDLE_VALUE;
void ThreadProc(void * param);


int Start_Thread();
int Stop_Thread();


void RTLSDRCallBack(unsigned char *buf, uint32_t len, void *ctx);
short *short_buf = NULL;

/* ExtIO Callback */
void (* WinradCallBack)(int, int, float, void *) = NULL;
#define WINRAD_SRCHANGE 100
#define WINRAD_LOCHANGE 101


static INT_PTR CALLBACK MainDlgProc(HWND, UINT, WPARAM, LPARAM);
HWND h_dialog=NULL;

int pll_locked=0;


extern "C"
bool  LIBRTL_API __stdcall InitHW(char *name, char *model, int& type)
{
	//MessageBox(NULL, TEXT("InitHW"),NULL, MB_OK);
	device_count = rtlsdr_get_device_count();
	if (!device_count) 
	{
		MessageBox(NULL,TEXT("No RTLSDR devices found"),
				   TEXT("ExtIO RTL"),
				   MB_ICONERROR | MB_OK);

		return FALSE;
	}

	connected_devices = new (std::nothrow) device[device_count];
	for( int i=0; i<device_count;i++)
		rtlsdr_get_device_usb_strings(0, connected_devices[i].vendor, connected_devices[i].product, connected_devices[i].serial);
	
	strcpy_s(name,15,connected_devices[0].vendor);
	strcpy_s(model,15,connected_devices[0].product);
	name[15]=0;
	model[15]=0;

	type = EXTIO_HWTYPE_16B; /* ExtIO type 16-bit samples */
	
//	fprintf(stderr, "São %d dispositivos. O primeiro é %s %s\n", device_count, name, model);

	return TRUE;
}

extern "C"
int LIBRTL_API __stdcall GetStatus()
{
	/* dummy function */		
    return 0;
}

extern "C"
bool  LIBRTL_API __stdcall OpenHW()
{
	//MessageBox(NULL, TEXT("OpenHW"),NULL, MB_OK);
	int r;
						
	r = rtlsdr_open(&dev,0);
	if(r < 0) {
//		MessageBox(NULL, TEXT("OpenHW Fudeu"),NULL, MB_OK);
		return FALSE;
	}
	
	r=rtlsdr_set_sample_rate(dev, samplerates[samplerate_default].value);
	if( r < 0)
		return FALSE;

	h_dialog=CreateDialog(hInst, MAKEINTRESOURCE(IDD_RTL_SETTINGS), NULL, (DLGPROC)MainDlgProc);
	ShowWindow(h_dialog,SW_HIDE);

	
	return TRUE;
}

extern "C"
long LIBRTL_API __stdcall SetHWLO(long freq)
{
	long r;

//	int t=Stop_Thread(); //Stop thread if there is one...

	r=rtlsdr_set_center_freq(dev, freq);
//	if (t==0)
//		Start_Thread();//and restart it if there was
	if (r!=pll_locked)
	{
		pll_locked=r;
		if (pll_locked==0)
			Static_SetText(GetDlgItem(h_dialog,IDC_PLL),TEXT("PLL LOCKED"));
		else
			Static_SetText(GetDlgItem(h_dialog,IDC_PLL),TEXT("PLL NOT LOCKED"));

		InvalidateRect(h_dialog, NULL, TRUE);
		UpdateWindow(h_dialog);
	}
	if (r!=0) {
		//MessageBox(NULL, TEXT("PLL not locked!"),TEXT("Error!"), MB_OK|MB_ICONERROR);
		return -1;
	}


	r=rtlsdr_get_center_freq(dev);

	if (r!=freq )
		WinradCallBack(-1,WINRAD_LOCHANGE,0,NULL);

	return 0;
}

extern "C"
int LIBRTL_API __stdcall StartHW(long freq)
{
	//MessageBox(NULL, TEXT("StartHW"),NULL, MB_OK);

	if (!dev) return -1;

	short_buf = new (std::nothrow) short[buffer_len];
	if (short_buf==0) {
		MessageBox(NULL, TEXT("Couldn't Allocate Buffer!"),TEXT("Error!"), MB_OK|MB_ICONERROR);
		return -1;
	}

	if(Start_Thread()<0)
	{
		delete short_buf;
		return -1;
	}

    SetHWLO(freq);

	EnableWindow(GetDlgItem(h_dialog,IDC_DEVICE),FALSE);

	return buffer_len/2;
}

extern "C"
long LIBRTL_API __stdcall GetHWLO()
{
	static long last_freq=100000000;
	long freq;

	//MessageBox(NULL, TEXT("GetHWLO"),NULL, MB_OK);
		
	freq=(long)rtlsdr_get_center_freq(dev);
	if (freq==0)
		return last_freq;
	last_freq=freq;
	return freq;
}


extern "C"
long LIBRTL_API __stdcall GetHWSR()
{
	//MessageBox(NULL, TEXT("GetHWSR"),NULL, MB_OK);
	return (long)rtlsdr_get_sample_rate(dev);
}

extern "C"
int LIBRTL_API __stdcall ExtIoGetSrates( int srate_idx, double * samplerate )
{
	//MessageBox(NULL, TEXT("ExtIoGetSrates"),NULL, MB_OK);

	if ( srate_idx < (sizeof(samplerates)/sizeof(samplerates[0])) )
	{
		samplerate=  &samplerates[srate_idx].value;
		return 0;
	}
	return 1;	// ERROR
}

extern "C"
int  LIBRTL_API __stdcall ExtIoGetActualSrateIdx(void)
{
	//MessageBox(NULL, TEXT("ExtIoGetActualSrateIdx"),NULL, MB_OK);
//						TCHAR str[255];
//						_stprintf(str, TEXT("Actual SR idx %d"), ComboBox_GetCurSel(GetDlgItem(h_dialog,IDC_SAMPLERATE)));
//						MessageBox(NULL, str, NULL, MB_OK);
	return  ComboBox_GetCurSel(GetDlgItem(h_dialog,IDC_SAMPLERATE));
}

extern "C"
int  LIBRTL_API __stdcall ExtIoSetSrate( int srate_idx )
{

//	TCHAR str[255];
//	_stprintf(str, TEXT("New SR idx %d"), srate_idx);
//	MessageBox(NULL, str, NULL, MB_OK);


	if (  srate_idx>=0 && srate_idx < (sizeof(samplerates)/sizeof(samplerates[0])) )
	{
//		MessageBox(NULL, TEXT("ExtIoSetSrate"),NULL, MB_OK);
		rtlsdr_set_sample_rate(dev, samplerates[srate_idx].value);
		ComboBox_SetCurSel(GetDlgItem(h_dialog,IDC_SAMPLERATE),srate_idx);
		WinradCallBack(-1,WINRAD_SRCHANGE,0,NULL);// Signal application
		return 0;
	}
	return 1;	// ERROR
}

extern "C"
int   LIBRTL_API __stdcall ExtIoGetSetting( int idx, char * description, char * value )
{
	//MessageBox(NULL, TEXT("ExtIoGetSetting"),NULL, MB_OK);
	switch ( idx )
	{
		case 0:	_snprintf( description, 1024, "%s", "SampleRateIdx" );	
				_snprintf( value, 1024, "%d", ComboBox_GetCurSel(GetDlgItem(h_dialog,IDC_SAMPLERATE)) );		
				return 0;
		case 1:	_snprintf( description, 1024, "%s", "Tuner_AGC" );		
				_snprintf( value, 1024, "%d", Button_GetCheck(GetDlgItem(h_dialog,IDC_TUNERAGC)) == BST_CHECKED ?1:0 );			
				return 0;
		case 2:	_snprintf( description, 1024, "%s", "RTL_AGC" );		
				_snprintf( value, 1024, "%d", Button_GetCheck(GetDlgItem(h_dialog,IDC_RTLAGC)) == BST_CHECKED ?1:0 );	
				return 0;
		case 3:	{_snprintf( description, 1024, "%s", "Frequency_Correction" );		
				TCHAR ppm[255];
				Edit_GetText(GetDlgItem(h_dialog,IDC_PPM), ppm, 255 );		
				_snprintf( value, 1024, "%d", _ttoi(ppm) );	
				return 0;}
		case 4:	{_snprintf( description, 1024, "%s", "Tuner_Gain" );		
				int pos=-SendMessage(GetDlgItem(h_dialog,IDC_GAIN),  TBM_GETPOS  , (WPARAM)0, (LPARAM)0);
				_snprintf( value, 1024, "%d",  pos );	
				return 0;}
		case 5:	_snprintf( description, 1024, "%s", "Buffer_Size" );	
				_snprintf( value, 1024, "%d", ComboBox_GetCurSel(GetDlgItem(h_dialog,IDC_BUFFER)) );		
				return 0;
		default:	return -1;	// ERROR
	}
	return -1;	// ERROR
}

extern "C"
void  LIBRTL_API __stdcall ExtIoSetSetting( int idx, const char * value )
{
	//MessageBox(NULL, TEXT(" ExtIoSetSetting"),NULL, MB_OK);
	int tempInt;

	switch ( idx )
	{
	case 0:		
				tempInt = atoi( value );
				if (  tempInt>=0 && tempInt < (sizeof(samplerates)/sizeof(samplerates[0])) )
				{
					samplerate_default=tempInt;
				}
				break;
	case 1:		tempInt = atoi( value );
				TunerAGC_default=tempInt?1:0;
				break;
	case 2:		tempInt = atoi( value );
				RTLAGC_default=tempInt?1:0;
				break;
	case 3:		tempInt = atoi( value );
				if (  tempInt>MIN_PPM && tempInt < MAX_PPM )
				{
					ppm_default=tempInt;
				}
				break;
	case 4:		tempInt = atoi( value );
		        gain_default=tempInt;
				break;
	case 5:		tempInt = atoi( value );
				if (  tempInt>=0 && tempInt < (sizeof(buffer_sizes)/sizeof(buffer_sizes[0])) )
				{
				buffer_default=tempInt;
				}
				break;
	}
}

extern "C"
void LIBRTL_API __stdcall StopHW()
{
	//MessageBox(NULL, TEXT("StopHW"),NULL, MB_OK);
	Stop_Thread();
	delete short_buf;
	EnableWindow(GetDlgItem(h_dialog,IDC_DEVICE),TRUE);
}

extern "C"
void LIBRTL_API __stdcall CloseHW()
{
	//MessageBox(NULL, TEXT("CloseHW"),NULL, MB_OK);
	rtlsdr_close(dev);
	dev=NULL;
	if (h_dialog!=NULL)
		DestroyWindow(h_dialog);
	
}

extern "C"
void LIBRTL_API __stdcall ShowGUI()
{
	ShowWindow(h_dialog,SW_SHOW);
	SetForegroundWindow(h_dialog);
	return;
}

extern "C"
void LIBRTL_API  __stdcall HideGUI()
{
	ShowWindow(h_dialog,SW_HIDE);
	return;
}


extern "C"
void LIBRTL_API __stdcall SetCallback(void (* myCallBack)(int, int, float, void *))
{

	WinradCallBack = myCallBack;

    return;
}


void RTLSDRCallBack(unsigned char *buf, uint32_t len, void *ctx)
{
	if(len == buffer_len)
	{
		short *short_ptr = (short*)&short_buf[0];
		unsigned char* char_ptr = buf;

		for(uint32_t i = 0 ; i < len;i++)
		{
			(*short_ptr) = ((short)(*char_ptr)) - 128;
			char_ptr ++;
			short_ptr ++;	
		}
		WinradCallBack(buffer_len,0,0,(void*)short_buf);
	}
}
int Start_Thread()
{
	//If already running, exit
	if(worker_handle != INVALID_HANDLE_VALUE)
		return -1;
	/* reset endpoint */
	if(rtlsdr_reset_buffer(dev) < 0)
		return -1;

	worker_handle = (HANDLE) _beginthread( ThreadProc, 0, NULL );
	if(worker_handle == INVALID_HANDLE_VALUE)
		return -1;

	SetThreadPriority(worker_handle, THREAD_PRIORITY_TIME_CRITICAL);
	return 0;
}

int Stop_Thread()
{
	if(worker_handle == INVALID_HANDLE_VALUE)
		return -1;
	rtlsdr_cancel_async(dev);
	// Wait 1s for thread to die
	WaitForSingleObject(worker_handle,INFINITE);
	CloseHandle(worker_handle);
	worker_handle=INVALID_HANDLE_VALUE;
	return 0;
}



void ThreadProc(void *p)
{


	/* Blocks until rtlsdr_cancel_async() is called */
	/* Use default number of buffers */
	
	rtlsdr_read_async(dev, (rtlsdr_read_async_cb_t)&RTLSDRCallBack, NULL, 0, buffer_len);
	
	_endthread();

}

static INT_PTR CALLBACK MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	static int n_gains;
	static int *gains;
	static HWND hGain;
	static int last_gain=0;
	static HBRUSH BRUSH_RED=CreateSolidBrush(RGB(255,0,0));
	static HBRUSH BRUSH_GREEN=CreateSolidBrush(RGB(0,255,0));

   	switch (uMsg)
    {
        case WM_INITDIALOG:
		{	
			Button_SetCheck(GetDlgItem(hwndDlg,IDC_TUNERAGC),TunerAGC_default?BST_CHECKED:BST_UNCHECKED);
			rtlsdr_set_tuner_gain_mode(dev,TunerAGC_default?0:1);

			Button_SetCheck(GetDlgItem(hwndDlg,IDC_RTLAGC),RTLAGC_default?BST_CHECKED:BST_UNCHECKED);
			rtlsdr_set_agc_mode(dev,RTLAGC_default?1:0);

			SendMessage(GetDlgItem(hwndDlg,IDC_PPM_S), UDM_SETRANGE  , (WPARAM)TRUE, (LPARAM)MAX_PPM | (MIN_PPM << 16));
			
			TCHAR tempStr[255];		
			_stprintf_s(tempStr,255, TEXT("%d"), ppm_default);
			Edit_SetText(GetDlgItem(hwndDlg,IDC_PPM), tempStr );
			rtlsdr_set_freq_correction(dev, ppm_default);
			
			for (int i=0; i<device_count;i++)
			{
				TCHAR str[255];
				_stprintf_s(str,255,  TEXT("(%d) - %S %S %S"),i+1, connected_devices[i].product,connected_devices[i].vendor,connected_devices[i].serial);
				ComboBox_AddString(GetDlgItem(hwndDlg,IDC_DEVICE),str);
			}
			ComboBox_SetCurSel(GetDlgItem(hwndDlg,IDC_DEVICE),0);

			for (int i=0; i<(sizeof(samplerates)/sizeof(samplerates[0]));i++)
			{
				ComboBox_AddString(GetDlgItem(hwndDlg,IDC_SAMPLERATE),samplerates[i].name);
//				MessageBox(NULL,sample_rates[i],TEXT("Mensagem"),MB_OK);
			}
			ComboBox_SetCurSel(GetDlgItem(hwndDlg,IDC_SAMPLERATE),samplerate_default);
  
			for (int i=0; i<(sizeof(buffer_sizes)/sizeof(int));i++)
			{
				TCHAR str[255];
				_stprintf_s(str,255, TEXT("%d kB"),buffer_sizes[i]); 
				ComboBox_AddString(GetDlgItem(hwndDlg,IDC_BUFFER),str);
			}
			ComboBox_SetCurSel(GetDlgItem(hwndDlg,IDC_BUFFER),buffer_default);
			buffer_len=buffer_sizes[buffer_default]*1024;

			n_gains = rtlsdr_get_tuner_gains(dev,NULL);
			gains = new int[n_gains];
			hGain = GetDlgItem(hwndDlg,IDC_GAIN);

			rtlsdr_get_tuner_gains(dev,gains);
			SendMessage(hGain, TBM_SETRANGEMIN , (WPARAM)TRUE, (LPARAM)-gains[n_gains-1]);
			SendMessage(hGain, TBM_SETRANGEMAX , (WPARAM)TRUE, (LPARAM)-gains[0]);
			int gain_d_index=0;
			for(int i=0; i<n_gains;i++)
			{
				SendMessage(hGain, TBM_SETTIC , (WPARAM)0, (LPARAM)-gains[i]);
				if (gain_default==gains[i]) gain_d_index=i;
			}
			SendMessage(hGain,  TBM_SETPOS  , (WPARAM)TRUE, (LPARAM)-gains[gain_d_index]);

			if (TunerAGC_default) {
				EnableWindow(hGain,FALSE);
				Static_SetText(GetDlgItem(hwndDlg,IDC_GAINVALUE),TEXT("AGC"));
			} else {
				int pos=-SendMessage(hGain,  TBM_GETPOS  , (WPARAM)0, (LPARAM)0);
				TCHAR str[255];
				_stprintf_s(str,255, TEXT("%2.1f dB"),(float) pos/10); 
				Static_SetText(GetDlgItem(hwndDlg,IDC_GAINVALUE),str);
				rtlsdr_set_tuner_gain(dev,gains[gain_d_index]);
			}

			return TRUE;
		}
        case WM_COMMAND:
            switch (GET_WM_COMMAND_ID(wParam, lParam))
            {
				case IDC_PPM:
					if(GET_WM_COMMAND_CMD(wParam, lParam) == EN_CHANGE)
                    { 
                        TCHAR ppm[255];
						Edit_GetText((HWND) lParam, ppm, 255 );
						if (!rtlsdr_set_freq_correction(dev, _ttoi(ppm)))
							WinradCallBack(-1,WINRAD_LOCHANGE,0,NULL);
					//	else
					//	{
					//		TCHAR str[255];
					//		_stprintf_s(str,255, TEXT("O valor é %d"), _ttoi(ppm));
					//		MessageBox(NULL, str, NULL, MB_OK);
					//	}
                    }
                    return TRUE;
                case IDC_RTLAGC:
				{
					if(Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) //it is checked
					{
						rtlsdr_set_agc_mode(dev,1);
						
//						MessageBox(NULL,TEXT("It is checked"),TEXT("Message"),0);
					}
					else //it has been unchecked
					{
						rtlsdr_set_agc_mode(dev,0);
						
			
//						MessageBox(NULL,TEXT("It is unchecked"),TEXT("Message"),0);
					}
					return TRUE;
				}
				case IDC_TUNERAGC:
				{
					if(Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) //it is checked
					{
						rtlsdr_set_tuner_gain_mode(dev,0);
						
						EnableWindow(hGain,FALSE);
						Static_SetText(GetDlgItem(hwndDlg,IDC_GAINVALUE),TEXT("AGC"));
//						MessageBox(NULL,TEXT("It is checked"),TEXT("Message"),0);
					}
					else //it has been unchecked
					{
						rtlsdr_set_tuner_gain_mode(dev,1);
						
						EnableWindow(hGain,TRUE);

						int pos=-SendMessage(hGain,  TBM_GETPOS  , (WPARAM)0, (LPARAM)0);
						TCHAR str[255];
						_stprintf_s(str,255, TEXT("%2.1f dB"),(float) pos/10); 
						Static_SetText(GetDlgItem(hwndDlg,IDC_GAINVALUE),str);
						
						rtlsdr_set_tuner_gain(dev,pos);
//						MessageBox(NULL,TEXT("It is unchecked"),TEXT("Message"),0);
					}
					return TRUE;
				}
				case IDC_SAMPLERATE:
					if(GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
                    { 
						rtlsdr_set_sample_rate(dev, samplerates[ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam))].value);
						WinradCallBack(-1,WINRAD_SRCHANGE,0,NULL);// Signal application

                        //TCHAR  ListItem[256];
						//ComboBox_GetLBText((HWND) lParam,ComboBox_GetCurSel((HWND) lParam),ListItem);
						//MessageBox(NULL, ListItem, TEXT("Item Selected"), MB_OK);
						//TCHAR str[255];
						//_stprintf(str, TEXT("O valor é %d"), samplerates[ComboBox_GetCurSel((HWND) lParam)].value);
						//MessageBox(NULL, str, NULL, MB_OK);
                    }
					if(GET_WM_COMMAND_CMD(wParam, lParam) ==  CBN_EDITUPDATE)
                    { 
                        TCHAR  ListItem[256];

						ComboBox_GetText((HWND) lParam,ListItem,256);
						//MessageBox(NULL, ListItem, TEXT("Item Selected"), MB_OK);

						TCHAR *endptr;
						double coeff = _tcstod(ListItem, &endptr);
						
						while (_istspace(*endptr)) ++endptr;

						int exp = 1;	
						switch (_totupper(*endptr)) {
							case 'K': exp = 1024; break;
							case 'M': exp = 1024*1024; break;
						}
						
						uint32_t newrate=coeff*exp;
						if (newrate>=MINRATE && newrate<=MAXRATE) {
							rtlsdr_set_sample_rate(dev, newrate);
							WinradCallBack(-1,WINRAD_SRCHANGE,0,NULL);// Signal application
						}

						//TCHAR str[255];
						//_stprintf(str, TEXT("O valor é %d"), newrate);
						//MessageBox(NULL, str, NULL, MB_OK);
                    }
					//MessageBox(NULL,TEXT("Bitrate"),TEXT("Mensagem"),MB_OK);
                    return TRUE;
				case IDC_BUFFER:
					if(GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
                    { 
						buffer_len=buffer_sizes[ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam))]*1024;
						WinradCallBack(-1,WINRAD_SRCHANGE,0,NULL);// Signal application
                    }
                    return TRUE;
				case IDC_DEVICE:
					if(GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
                    { 
						rtlsdr_close(dev);
						dev=NULL;
						if(rtlsdr_open(&dev,ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam))) < 0) 
						{
							MessageBox(NULL,TEXT("Cound't open device!"),
										TEXT("ExtIO RTL"),
										MB_ICONERROR | MB_OK);
							return TRUE;
						}
						rtlsdr_set_sample_rate(dev, samplerates[samplerate_default].value);
                    }
                    return TRUE;

			}
            break;
		case WM_VSCROLL:
			//if (LOWORD(wParam)!=TB_THUMBTRACK && LOWORD(wParam)!=TB_ENDTRACK)

			if ((HWND)lParam==hGain)
			{
				
				int pos = -SendMessage(hGain,  TBM_GETPOS  , (WPARAM)0, (LPARAM)0);
				for (int i=0;i<n_gains-1;i++)
					if (pos>gains[i] && pos<gains[i+1]) 
						if((pos-gains[i])<(gains[i+1]-pos) && (LOWORD(wParam)!=TB_LINEUP) || (LOWORD(wParam)==TB_LINEDOWN))
							pos=gains[i];
						else
							pos=gains[i+1];
				
				SendMessage(hGain,  TBM_SETPOS  , (WPARAM)TRUE, (LPARAM)-pos);
				TCHAR str[255];
				_stprintf_s(str,255, TEXT("%2.1f  dBm"),(float) pos/10); 
				Static_SetText(GetDlgItem(hwndDlg,IDC_GAINVALUE),str);
				
				if (pos!=last_gain)
				{
					last_gain=pos;
					rtlsdr_set_tuner_gain(dev,pos);
				}			
				
				/* MessageBox(NULL, str, NULL, MB_OK);*/
				return TRUE;
			}
			if ((HWND)lParam==GetDlgItem(hwndDlg,IDC_PPM_S))
			{
	//			MessageBox(NULL,TEXT("ola"),NULL, MB_OK);
				return TRUE;
			}
			/*	TCHAR str[255];
				_stprintf(str, TEXT("%d"),LOWORD(wParam)); 
				MessageBox(NULL, str, NULL, MB_OK);*/
			break;


		//case WM_SYSCOMMAND:
		//	switch (wParam & 0xFFF0)
  //          {
		//		case SC_SIZE:
		//		case SC_MINIMIZE:
		//		case SC_MAXIMIZE:				
  //                  return TRUE; 
  //          }
  //          break;
        case WM_CLOSE:
			ShowWindow(h_dialog,SW_HIDE);
            return TRUE;
			break;
		case WM_DESTROY:
			delete[] gains;
			h_dialog=NULL;
            return TRUE;
			break;


		case WM_CTLCOLORSTATIC:
			if ( IDC_PLL == GetDlgCtrlID((HWND)lParam))
			{
				HDC hdc = (HDC)wParam;
				if (pll_locked==0)
				{
					SetBkColor(hdc, RGB(0,255,0));
					return (INT_PTR)BRUSH_GREEN;
				} else {
					SetBkColor(hdc, RGB(255,0,0));
					return (INT_PTR)BRUSH_RED;
				}
			}
			break;

        /*
         * TODO: Add more messages, when needed.
         */
    }

    return FALSE;
}