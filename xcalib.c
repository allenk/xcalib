/*
 * xcalib - download vcgt gamma tables to your X11 video card
 *
 * (c) 2004-2005 Stefan Doehla <stefan AT doehla DOT de>
 *
 * This program is GPL-ed postcardware! please see README
 *
 * It is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA.
 */

/*
 * xcalib is a tiny tool to load the content of vcgt-Tags in ICC
 * profiles to the video card's gamma ramp. It does work with most
 * video card drivers except the generic VESA driver.
 *
 * There are three ways to parse an ICC profile:
 * - use Graeme Gill's icclib (bundled)
 * - use a patched version of Marti Maria's LCMS (patches included)
 * - use internal parsing routines for vcgt-parsing only
 *
 * Using icclib is known to work best, patched LCMS has the
 * advantage of gamma ramp interpolation and the internal routine
 * is perfect for low overhead versions of xcalib.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>

/* for X11 VidMode stuff */
#include <X11/Xos.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/xf86vmode.h>

/* for icc profile parsing */
#ifdef ICCLIB
# include <icc.h>
#elif PATCHED_LCMS
# include <lcms.h>
#endif

#include <math.h>

/* system gamma is 2.2222 on most UNIX systems
 * MacOS uses 1.8, MS-Windows 2.2
 * XFree gamma 1.0 is gamma 2.222 at the output*/
#define SYSTEM_GAMMA  2.222222

/* prototypes */
void error (char *fmt, ...), warning (char *fmt, ...), message(char *fmt, ...);
#ifdef PATCHED_LCMS
BOOL ReadVCGT(cmsHPROFILE hProfile, LPGAMMATABLE * pRTable, LPGAMMATABLE * pGTable, LPGAMMATABLE * pBTable, unsigned int nEntries);
static icInt32Number  SearchTag(LPLCMSICCPROFILE Profile, icTagSignature sig);
#endif


/* internal state struct */
struct xcalib_state_t {
  unsigned int verbose;
#ifdef PATCHED_LCMS
  LPGAMMATABLE rGammaTable;
  LPGAMMATABLE gGammaTable;
  LPGAMMATABLE bGammaTable;
#endif
} xcalib_state;


void
usage (void)
{
  fprintf (stdout, "usage:  xcalib [-options] ICCPROFILE\n");
  fprintf (stdout, "Copyright (C) 2004-2005 Stefan Doehla <stefan AT doehla DOT de>\n");
  fprintf (stdout, "THIS PROGRAM COMES WITH ABSOLUTELY NO WARRANTY!\n");
  fprintf (stdout, "\n");
  fprintf (stdout, "where the available options are:\n");
  fprintf (stdout, "    -display <host:dpy>     or -d\n");
  fprintf (stdout, "    -screen <screen-#>      or -s\n");
  fprintf (stdout, "    -clear                  or -c\n");
  fprintf (stdout, "    -noaction               or -n\n");
  fprintf (stdout, "    -verbose                or -v\n");
  fprintf (stdout, "    -help                   or -h\n");
  fprintf (stdout, "    -version\n");
  fprintf (stdout, "\n");
  fprintf (stdout,
	   "last parameter must be an ICC profile containing a vcgt-tag\n");
  fprintf (stdout, "\n");
  fprintf (stdout, "Example: ./xcalib -d :0 -s 0 -v gamma_1_0.icc\n");
  fprintf (stdout, "\n");
  fprintf (stdout, "\n");
  exit (1);
}

/*
 * FUNCTION read_vcgt_from_profile
 *
 * this is a parser for the vcgt tag of ICC profiles which tries to
 * resemble most of the functionality of Graeme Gill's icclib.
 *
 * It is not completely finished yet, so you might be better off using
 * the LCMS or icclib version of xcalib. It is not Big-Endian-safe!
 */
int
read_vcgt_from_profile(const char * filename, u_int16_t ** rRamp, u_int16_t ** gRamp,
		       u_int16_t ** bRamp, unsigned int * nEntries)
{
  FILE * fp;
  unsigned int bytesRead;
  unsigned int numTags=0;
  unsigned int tagName=0;
  unsigned int tagOffset=0;
  unsigned int tagSize=0;
  unsigned char cTmp[4];
  unsigned int uTmp;
  unsigned int gammaType;

  u_int16_t * redRamp, * greenRamp, * blueRamp;
  unsigned int ratio=0;
  /* formula */
  float rGamma, rMin, rMax;
  float gGamma, gMin, gMax;
  float bGamma, bMin, bMax;
  int i=0;
  /* table */
  unsigned int numChannels=0;
  unsigned int numEntries=0;
  unsigned int entrySize=0;
  int j=0;

  if(filename) {
    fp = fopen(filename, "rb");
    if(!fp)
      return -1;
  } else
    return -1;
  /* skip header */
  fseek(fp, 0+128, SEEK_SET);
  /* check num of tags in current profile */
  bytesRead = fread(cTmp, 1, 4, fp);
  numTags = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
  //!message("%u", numTags);
  for(i=0; i<numTags; i++) {
    bytesRead = fread(cTmp, 1, 4, fp);
    tagName = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
    bytesRead = fread(cTmp, 1, 4, fp);
    tagOffset = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
    bytesRead = fread(cTmp, 1, 4, fp);
    tagSize = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
    if(!bytesRead)
      goto cleanup_fileparser;
    if(tagName == 'vcgt')
    {
      fseek(fp, 0+tagOffset, SEEK_SET);
      message("vcgt found\n");
      bytesRead = fread(cTmp, 1, 4, fp);
      tagName = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
      if(tagName != 'vcgt')
        error("invalid content of table vcgt, starting with %x",
		tagName);
        //! goto cleanup_fileparser;
      bytesRead = fread(cTmp, 1, 4, fp);
      bytesRead = fread(cTmp, 1, 4, fp);
      gammaType = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
      /* VideoCardGammaFormula */
      //!message("Gamma Type:  %u", gammaType);
      if(gammaType==1) {
        bytesRead = fread(cTmp, 1, 4, fp);
        uTmp = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
        rGamma = (float)uTmp/65536.0;
        bytesRead = fread(cTmp, 1, 4, fp);
        uTmp = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
        rMin = (float)uTmp/65536.0;
        bytesRead = fread(cTmp, 1, 4, fp);
        uTmp = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
        rMax = (float)uTmp/65536.0;
        bytesRead = fread(cTmp, 1, 4, fp);
        uTmp = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
        gGamma = (float)uTmp/65536.0;
        bytesRead = fread(cTmp, 1, 4, fp);
        uTmp = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
        gMin = (float)uTmp/65536.0;
        bytesRead = fread(cTmp, 1, 4, fp);
        uTmp = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
        gMax = (float)uTmp/65536.0;
        bytesRead = fread(cTmp, 1, 4, fp);
        uTmp = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
        bGamma = (float)uTmp/65536.0;
        bytesRead = fread(cTmp, 1, 4, fp);
        uTmp = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
        bMin = (float)uTmp/65536.0;
        bytesRead = fread(cTmp, 1, 4, fp);
        uTmp = cTmp[3]+cTmp[2]*256+cTmp[1]*65536+cTmp[0]*16777216;
        bMax = (float)uTmp/65536.0;
	
	message("Red:   Gamma %f \tMin %f \tMax %f\n", rGamma, rMin, rMax);
	message("Green: Gamma %f \tMin %f \tMax %f\n", gGamma, gMin, gMax);
	message("Blue:  Gamma %f \tMin %f \tMax %f\n", bGamma, bMin, bMax);

	*rRamp = (unsigned short *) malloc ((*nEntries) * sizeof (unsigned short));
        *gRamp = (unsigned short *) malloc ((*nEntries) * sizeof (unsigned short));
        *bRamp = (unsigned short *) malloc ((*nEntries) * sizeof (unsigned short));
	for(j=0; j<*nEntries; j++) {
          rRamp[0][j] = 65563 *
            (double) pow ((double) j /
            (double) (*nEntries),
            rGamma *
            (double) SYSTEM_GAMMA);
          gRamp[0][j] = 65563 *
            (double) pow ((double) j /
            (double) (*nEntries),
            gGamma *
            (double) SYSTEM_GAMMA);
          bRamp[0][j] = 65563 *
            (double) pow ((double) j /
            (double) (*nEntries),
            bGamma *
            (double) SYSTEM_GAMMA);
	}	
      }
      /* VideoCardGammaTable */
      else if(gammaType==0) {
        bytesRead = fread(cTmp, 1, 2, fp);
	numChannels = cTmp[1]+cTmp[0]*256;
	bytesRead = fread(cTmp, 1, 2, fp);
        numEntries = cTmp[1]+cTmp[0]*256;
	bytesRead = fread(cTmp, 1, 2, fp);
        entrySize = cTmp[1]+cTmp[0]*256;

	if(numChannels!=3)          /* assume we have always RGB */
          goto cleanup_fileparser;

        redRamp = (unsigned short *) malloc ((numEntries) * sizeof (unsigned short));
        greenRamp = (unsigned short *) malloc ((numEntries) * sizeof (unsigned short));
        blueRamp = (unsigned short *) malloc ((numEntries) * sizeof (unsigned short));
	{		  
	  for(j=0; j<numEntries; j++) {
            switch(entrySize) {
              case 1:
                bytesRead = fread(cTmp, 1, 1, fp);
                redRamp[j]= cTmp[0];
              case 2:
                bytesRead = fread(cTmp, 1, 2, fp);
                redRamp[j]= cTmp[1] + 256*cTmp[0];
	    }
	  }
          for(j=0; j<numEntries; j++) {
            switch(entrySize) {
              case 1:
                bytesRead = fread(cTmp, 1, 1, fp);
                greenRamp[j]= cTmp[0];
              case 2:
                bytesRead = fread(cTmp, 1, 2, fp);
                greenRamp[j]= cTmp[1] + 256*cTmp[0];
	    }
	  }
          for(j=0; j<numEntries; j++) {
            switch(entrySize) {
              case 1:
                bytesRead = fread(cTmp, 1, 1, fp);
                blueRamp[j]= cTmp[0];
              case 2:
                bytesRead = fread(cTmp, 1, 2, fp);
                blueRamp[j]= cTmp[1] + 256*cTmp[0];
	    }
          }
	}
	/* interpolate if vcgt size doesn't match video card's gamma table */
	if(*nEntries == numEntries) {
	  *rRamp = redRamp;
	  *gRamp = greenRamp;
	  *bRamp = blueRamp;
	}
	else if(*nEntries < numEntries) {
	  ratio = (unsigned int)(numEntries / (*nEntries));
          *rRamp = (unsigned short *) malloc ((*nEntries) * sizeof (unsigned short));
          *gRamp = (unsigned short *) malloc ((*nEntries) * sizeof (unsigned short));
          *bRamp = (unsigned short *) malloc ((*nEntries) * sizeof (unsigned short));
	  for(j=0; j<*nEntries; j++) {
	    rRamp[0][j] = redRamp[ratio*j];
	    gRamp[0][j] = greenRamp[ratio*j];
	    bRamp[0][j] = blueRamp[ratio*j];
	  }
	}
	/* interpolation of zero order - TODO: at least bilinear */
	else if(*nEntries > numEntries) {
	  ratio = (unsigned int)((*nEntries) / numEntries);
          *rRamp = (unsigned short *) malloc ((*nEntries) * sizeof (unsigned short));
          *gRamp = (unsigned short *) malloc ((*nEntries) * sizeof (unsigned short));
          *bRamp = (unsigned short *) malloc ((*nEntries) * sizeof (unsigned short));
	  for(j=0; j<*nEntries; j++) {
	    rRamp[0][j] = redRamp[j/ratio];
	    gRamp[0][j] = greenRamp[j/ratio];
	    bRamp[0][j] = blueRamp[j/ratio];
	  }
	}
      }
      goto cleanup_fileparser;
    }
  }
  
cleanup_fileparser:
  fclose(fp);
  return 0;
}

int
main (int argc, char *argv[])
{
  int fa, nfa;			/* argument we're looking at */
  char in_name[256];
  char tag_name[40] = { '\000' };
  int verb = 2;
  int search = 0;
  int ecount = 1;		/* Embedded count */
  int offset = 0;		/* Offset to read profile from */
  int found;
#ifdef ICCLIB
  icmFile *fp, *op;
  icc *icco;			/* ICC object */
#elif PATCHED_LCMS
  cmsHPROFILE hDisplayProfile;
#endif
  struct xcalib_state_t * x = NULL;
  int rv = 0;
  u_int16_t *r_ramp, *g_ramp, *b_ramp;
  int i;
  int clear = 0;
  int donothing = 0;
  XF86VidModeGamma gamma;

  /* X11 */
  Display *dpy = NULL;
  int screen = -1;
  unsigned int ramp_size;
  unsigned int ramp_scaling;
  char *displayname;

  xcalib_state.verbose = 0;
  x = &xcalib_state;

  /* begin program part */

  /* command line parsing */
  
  if (argc < 2)
    usage ();

  for (i = 1; i < argc; ++i)
    {
      /* help */
      if (!strcmp (argv[i], "-h") || !strcmp (argv[i], "-help"))
	{
	  usage ();
	  exit (0);
	}
      /* verbose mode */
      if (!strcmp (argv[i], "-v") || !strcmp (argv[i], "-verbose"))
	{
	  xcalib_state.verbose = 1;
	  continue;
	}
      /* version */
      if (!strcmp (argv[i], "-version"))
      {
        fprintf(stdout, "xcalib 0.3\n");
        exit (0);
      }
      /* X11 display */
      if (!strcmp (argv[i], "-d") || !strcmp (argv[i], "-display"))
	{
	  if (++i >= argc)
	    usage ();
	  displayname = argv[i];
	  continue;
	}
      /* X11 screen */
      if (!strcmp (argv[i], "-s") || !strcmp (argv[i], "-screen"))
	{
	  if (++i >= argc)
	    usage ();
	  screen = atoi (argv[i]);
	  continue;
	}
      /* clear gamma lut */
      if (!strcmp (argv[i], "-c") || !strcmp (argv[i], "-clear"))
	{
	  clear = 1;
	  continue;
	}
      /* do not alter video-LUTs : work's best in conjunction with -v! */
      if (!strcmp (argv[i], "-n") || !strcmp (argv[i], "-noaction"))
        {
          donothing = 1;
          continue;
        }
      if (i != argc - 1 && !clear)
	usage ();
      if (!clear)
	strcpy (in_name, argv[i]);
    }

  if (!clear)
    {
#ifdef ICCLIB
      /* Open up the file for reading */
      if ((fp = new_icmFileStd_name (in_name, "r")) == NULL)
	error ("Can't open file '%s'", in_name);

      if ((icco = new_icc ()) == NULL)
	error ("Creation of ICC object failed");
#elif PATCHED_LCMS
      hDisplayProfile = cmsOpenProfileFromFile(in_name, "r");
      if(hDisplayProfile == NULL)
        error ("Can't open ICC profile");
#endif 
    }
  
  /* X11 initializing */
  if ((dpy = XOpenDisplay (displayname)) == NULL)
    {
      error ("Can't open display %s", XDisplayName (displayname));
    }
  else if (screen == -1)
    screen = DefaultScreen (dpy);

  /* clean gamma table if option set */
  gamma.red = 1.0;
  gamma.green = 1.0;
  gamma.blue = 1.0;
  if (clear)
    {
      if (!XF86VidModeSetGamma (dpy, screen, &gamma))
	{
	  XCloseDisplay (dpy);
	  error ("Unable to reset display gamma");
	}
      goto cleanupX;
    }
  
  /* get number of entries for gamma ramps */
  if (!XF86VidModeGetGammaRampSize (dpy, screen, &ramp_size))
    {
      XCloseDisplay (dpy);
      error ("Unable to query gamma ramp size");
    }

#if !defined(ICCLIB) && !defined(PATCHED_LCMS)
read_vcgt_from_profile(in_name, &r_ramp, &g_ramp, &b_ramp, &ramp_size);
#endif 

#ifdef ICCLIB

  do
    {
      found = 0;

      /* Dumb search for magic number */
      if (search)
        {
          int fc = 0;
          char c;

          if (fp->seek (fp, offset) != 0)
            break;

          while (found == 0)
            {
              if (fp->read (fp, &c, 1, 1) != 1)
                {
                  break;
                }
              offset++;

              switch (fc)
                {
                case 0:
                  if (c == 'a')
                    fc++;
                  else
                    fc = 0;
                  break;
                case 1:
                  if (c == 'c')
                    fc++;
                  else
                    fc = 0;
                  break;
                case 2:
                  if (c == 's')
                    fc++;
                  else
                    fc = 0;
                  break;
                case 3:
                  if (c == 'p')
                    {
                      found = 1;
                      offset -= 40;
                    }
                  else
                    fc = 0;
                  break;
                }
            }
        }

      if (search == 0 || found != 0)
        {
          ecount++;

          if ((rv = icco->read (icco, fp, offset)) != 0)
            error ("%d, %s", rv, icco->err);

          {
            /* we only search for vcgt tag */
            icTagSignature sig = icSigVideoCardGammaTag;

            /* Try and locate that particular tag */
            if ((rv = icco->find_tag (icco, sig)) != 0)
              {
                if (rv == 1)
                  warning ("found vcgt-tag but inconsistent values",
                           tag_name);
                else
                  error ("can't find tag '%s' in file", tag2str (sig));
              }
            else
              {
                icmVideoCardGamma *ob;

                if ((ob =
                     (icmVideoCardGamma *) icco->read_tag (icco,
                                                           sig)) == NULL)
                  {
                    warning
                      ("Failed to read video card gamma table: %d, %s",
                       tag_name, icco->errc, icco->err);
                  }
                else
                  {
                    if (ob->ttype != icSigVideoCardGammaType)
                      warning
                        ("Video card gamma table is in inconsistent state");
                    switch ((int) ob->tagType)
                      {
                        /* video card gamme table: can be loaded directly to X-server if appropriately scaled */
                      case icmVideoCardGammaTableType:
                            message ("channels:        %d\n",
                                     ob->u.table.channels);
                            message ("entry size:      %dbits\n",
                                     ob->u.table.entrySize * 8);
                            message ("entries/channel: %d\n",
                                     ob->u.table.entryCount);

                        ramp_scaling = ob->u.table.entryCount / ramp_size;

                        /* we must interpolate in case of bigger gamma ramps of X-server than
                         * the vcg-table does contain. This is something that needs to be done
                         * in a later version TODO */
                        if(ob->u.table.entryCount < ramp_size)
                          {
                            error("vcgt-tag does not contain enough data for this Video-LUT");
                          }

                        r_ramp =
                          (unsigned short *) malloc ((ramp_size + 1) *
                                                     sizeof (unsigned short));
                        g_ramp =
                          (unsigned short *) malloc ((ramp_size + 1) *
                                                     sizeof (unsigned short));
                        b_ramp =
                          (unsigned short *) malloc ((ramp_size + 1) *
                                                     sizeof (unsigned short));

                        /* TODO: allow interpolation for non-integer divisors in
                         * between Video-LUT and X gamma ramp*/
                        switch (ob->u.table.entrySize)
                          {
                          case (1):     /* 8 bit */
                            for (i = 0; i < ramp_size; i++)
                              r_ramp[i] =
                                (unsigned short) ((char *) ob->u.
                                                  table.data)[i *
                                                              ramp_scaling];
                            for (; i < 2 * ramp_size; i++)
                              g_ramp[i - ramp_size] =
                                (unsigned short) ((char *) ob->u.
                                                  table.data)[i *
                                                              ramp_scaling];
                            for (; i < 3 * ramp_size; i++)
                              b_ramp[i - 2 * ramp_size] =
                                (unsigned short) ((char *) ob->u.
                                                  table.data)[i *
                                                              ramp_scaling];
                            break;
                          case (2):     /* 16 bit */
                            for (i = 0; i < ramp_size; i++)
                              r_ramp[i] =
                                (unsigned short) ((short *) ob->u.
                                                  table.data)[i *
                                                              ramp_scaling];
                            for (; i < (2 * ramp_size); i++)
                              g_ramp[i - ramp_size] =
                                (unsigned short) ((short *) ob->u.
                                                  table.data)[i *
                                                              ramp_scaling];
                            for (; i < (3 * ramp_size); i++)
                              b_ramp[i - 2 * ramp_size] =
                                (unsigned short) ((short *) ob->u.
                                                  table.data)[i *
                                                              ramp_scaling];
                            break;
                          }

                        break;
                        /* gamma formula type: currently no minimum/maximum value TODO*/
                      case icmVideoCardGammaFormulaType:
                        r_ramp =
                          (unsigned short *) malloc ((ramp_size + 1) *
                                                     sizeof (unsigned short));
                        g_ramp =
                          (unsigned short *) malloc ((ramp_size + 1) *
                                                     sizeof (unsigned short));
                        b_ramp =
                          (unsigned short *) malloc ((ramp_size + 1) *
                                                     sizeof (unsigned short));

                        ramp_scaling = 65563 / ramp_size;

                        /* maths for gamma calculation: use system gamma as reference */
                        for (i = 0; i < ramp_size; i++)
                          {
                            r_ramp[i] =
                              65563 *
                              (double) pow ((double) i /
                                            (double) ramp_size,
                                            ob->u.formula.redGamma *
                                            (double) SYSTEM_GAMMA);
                            g_ramp[i] =
                              65563 *
                              (double) pow ((double) i /
                                            (double) ramp_size,
                                            ob->u.formula.greenGamma *
                                            (double) SYSTEM_GAMMA);
                            b_ramp[i] =
                              65563 *
                              (double) pow ((double) i /
                                            (double) ramp_size,
                                            ob->u.formula.blueGamma *
                                            (double) SYSTEM_GAMMA);
                          }
                        break;  /* gamma formula */
                      }
                    /* debug stuff: print ramps */
                        for (i = 0; i < ramp_size - 1; i++)
                          {
                            if (r_ramp[i + 1] < r_ramp[i])
                              warning ("nonsense content in red gamma table");
                            if (g_ramp[i + 1] < g_ramp[i])
                              warning
                                ("nonsense content in green gamma table");
                            if (b_ramp[i + 1] < b_ramp[i])
                              warning
                                ("nonsense content in blue gamma table");
                            message ("%d\t%d\t%d\n", r_ramp[i],
                                     g_ramp[i], b_ramp[i]);
                          }
                        message ("%d\t%d\t%d\n",
                                 r_ramp[ramp_size - 1],
                                 g_ramp[ramp_size - 1],
                                 b_ramp[ramp_size - 1]);
                  }
              }
          }
          offset += 128;
        }
    }
  while (found != 0);

#elif PATCHED_LCMS
  x->rGammaTable = x->gGammaTable =x->bGammaTable = NULL;
  x->rGammaTable = cmsAllocGamma(ramp_size);
  x->gGammaTable = cmsAllocGamma(ramp_size);
  x->bGammaTable = cmsAllocGamma(ramp_size);

  if(!cmsTakeVideoCardGammaTable(hDisplayProfile, &(x->rGammaTable), &(x->gGammaTable), &(x->bGammaTable) ) )
    error("xcalib exited due to error in reading vcgt");
   
  /* XVidMode gamma ramps have same size than LCMS GammaTable-content */
  r_ramp = x->rGammaTable->GammaTable;
  g_ramp = x->gGammaTable->GammaTable;
  b_ramp = x->bGammaTable->GammaTable;

#endif

  for(i=0; i<ramp_size; i++)
    message("%x %x %x\n", r_ramp[i], g_ramp[i], b_ramp[i]);

  if(!donothing)
    /* write gamma ramp to X-server */
    if (!XF86VidModeSetGammaRamp
        (dpy, screen, ramp_size, r_ramp, g_ramp, b_ramp))
      {
        warning ("Unable to calibrate display");
      }

#ifdef ICCLIB
  icco->del (icco);
  fp->del (fp);
#elif PATCHED_LCMS
  if(x->rGammaTable!=NULL)
    cmsFreeGamma(x->rGammaTable);
  if(x->gGammaTable!=NULL)
    cmsFreeGamma(x->gGammaTable);
  if(x->bGammaTable!=NULL)
    cmsFreeGamma(x->bGammaTable);
#endif

  message ("X-LUT size: %d\n", ramp_size);

cleanupX:
  XCloseDisplay (dpy);

  return 0;
}

/* Basic printf type error() and warning() routines */

/* errors are printed to stderr */
void
error (char *fmt, ...)
{
  va_list args;

  fprintf (stderr, "Error - ");
  va_start (args, fmt);
  vfprintf (stderr, fmt, args);
  va_end (args);
  fprintf (stderr, "\n");
  exit (-1);
}

/* warnings are printed to stdout */
void
warning (char *fmt, ...)
{
  va_list args;

  fprintf (stdout, "Warning - ");
  va_start (args, fmt);
  vfprintf (stdout, fmt, args);
  va_end (args);
  fprintf (stdout, "\n");
}

/* messages are printed only if the verbose flag is set */
void
message (char *fmt, ...)
{
  va_list args;

  if(xcalib_state.verbose) {
  va_start (args, fmt);
  vfprintf (stdout, fmt, args);
  va_end (args);
  }
}

