///////////////////////////////////////////////////////////////////////////////
// Name:        makefont.cpp
// Purpose:     Utility for creating font metrics file for use with wxPdfDocument
// Author:      Ulrich Telle
// Created:     2005-09-05
// Copyright:   (c) Ulrich Telle
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

// For compilers that support precompilation, includes "wx/wx.h".
#include "wx/wxprec.h"

#ifdef __BORLANDC__
#pragma hdrstop
#endif

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include <wx/cmdline.h>
#include <wx/log.h>
#include <wx/dynarray.h>
#include <wx/tokenzr.h>
#include <wx/mstream.h>
#include <wx/stdpaths.h>
#include <wx/txtstrm.h>
#include <wx/wfstream.h>
#include <wx/xml/xml.h>
#include <wx/zstream.h>

#include "wx/pdfdoc.h"
#include "wx/pdfarraytypes.h"
#include "wx/pdffontparsertruetype.h"
#include "wx/pdffontdata.h"
#include "wx/pdffontdataopentype.h"
#include "wx/pdffontdatatruetype.h"
#include "wx/pdffontdatatype1.h"
#include "wx/pdffontmanager.h"

static int
CompareInts(int n1, int n2)
{
  return n1 - n2;
}

class GlyphListEntry
{
public:
  GlyphListEntry() {};
  ~GlyphListEntry() {};
  int m_gid;
  int m_uid;
};

WX_DEFINE_SORTED_ARRAY(GlyphListEntry*, GlyphList);

static int
CompareGlyphListEntries(GlyphListEntry* item1, GlyphListEntry* item2)
{
  return item1->m_gid - item2->m_gid;
}

WX_DECLARE_HASH_MAP(long, wxString, wxIntegerHash, wxIntegerEqual, CTGMap);
WX_DECLARE_STRING_HASH_MAP(wxString, FixGlyphMap);

class MakeFont : public wxAppConsole
{
public:
  bool OnInit();
  int OnRun();
  int OnExit();

protected:
  bool MakeFontAFM(const wxString& fontFileName, const wxString& afmFileName,
                   const wxString& encoding = wxS("cp1252"),
                   const wxString& type = wxS("TrueType"),
                   const wxString& patch = wxEmptyString);
#if wxUSE_UNICODE
  bool MakeFontUFM(const wxString& fontFileName,
                   const wxString& ufmFileName,
                   const wxString& type = wxS("TrueType"),
                   const wxString& volt = wxS(""));
  bool MakeFontImmediate(const wxString& fontFileName);

  void WriteStreamBuffer(wxOutputStream& stream, const char* buffer);
  void WriteToUnicode(GlyphList& glyphs, wxMemoryOutputStream& toUnicode);
#endif
  bool ReadMap(const wxString& enc, CTGMap& cc2gn);
  short ReadShort(wxInputStream* stream);
  int ReadInt(wxInputStream* stream);
  void CheckTTF(const wxString& fileName, 
                bool& embeddingAllowed, bool& subsettingAllowed, 
                int& cffOffset, int& cffLength);
  void CreateFontMetricsFile(const wxString& xmlFileName, wxPdfFontData& font, bool includeGlyphInfo);

private:
  wxString m_version;
  bool     m_unicode;
  bool     m_immediate;
  wxString m_afmFile;
  wxString m_ufmFile;
  wxString m_fontFile;
  wxString m_encoding;
  wxString m_patchFile;
  wxString m_fontType;
  wxString m_volt;
  wxString m_outputDir;
};

/// Fast string search (KMP method): initialization
static int*
makeFail(const char* target, int tlen)
{
  int t = 0;
  int s, m;
  m = tlen;
  int* f = new int[m+1];
  f[1] = 0;
  for (s = 1; s < m; s++)
  {
    while ((t > 0) && (target[s] != target[t]))
    {
      t = f[t];
    }
    if (target[t] == target[s])
    {
      t++;
      f[s+1] = t;
    }
    else
    {
      f[s+1] = 0;
    }
  }
  return f;
}

/// Fast string search (KMP method)
static int
findString(const char* src, int slen, const char* target, int tlen, int* f)
{
  int s = 0;
  int i;
  int m = tlen;
  for (i = 0; i < slen; i++)
  {
    while ( (s > 0) && (src[i] != target[s]))
    {
      s = f[s];
    }
    if (src[i] == target[s]) s++;
    if (s == m) return (i-m+1);
  }
  return -1;
}

/// Read encoding map
bool
MakeFont::ReadMap(const wxString& enc, CTGMap& cc2gn)
{
  //Read a map file
  wxString mapFileName = enc.Lower() + wxS(".map");
  wxFileInputStream mapFile(mapFileName);
  if (!mapFile.Ok())
  {
    wxLogMessage(wxS("Error: Unable to read map file '") + mapFileName + wxS("'."));
    return false;
  }

  wxTextInputStream text(mapFile);
  wxString charcode, unicode, glyphname;

  cc2gn.clear();
  while (!mapFile.Eof())
  {
    text >> charcode >> unicode >> glyphname;
    if (!mapFile.Eof() && charcode.Length() > 0)
    {
      if (charcode[0] == wxS('!'))
      {
        int cc = wxHexToDec(charcode.Mid(1));
        cc2gn[cc] = glyphname;
      }
    }
  }

  int i;
  for (i = 0; i <= 255; i++)
  {
    CTGMap::iterator iter = cc2gn.find(i);
    if (iter == cc2gn.end())
    {
      cc2gn[i] = wxS(".notdef");
    }
  }
  return true;
}

/// Read a 2-byte integer from file (big endian)
short
MakeFont::ReadShort(wxInputStream* stream)
{
  short i16;
  stream->Read(&i16, 2);
  return wxINT16_SWAP_ON_LE(i16);
}

/// Read a 4-byte integer from file (big endian)
int
MakeFont::ReadInt(wxInputStream* stream)
{
  int i32;
  stream->Read(&i32, 4);
  return wxINT32_SWAP_ON_LE(i32);
}

/// Check TrueType font file whether font license allows embedding
void
MakeFont::CheckTTF(const wxString& fileName, bool& embeddingAllowed, bool& subsettingAllowed, int& cffOffset, int& cffLength)
{
  embeddingAllowed = false;
  subsettingAllowed = false;
  cffOffset = -1;
  cffLength = 0;
  if (fileName.Length() == 0)
  {
    return;
  }
  wxFileInputStream ttfFile(fileName);
  if (!ttfFile.Ok())
  {
    // Can't open file
    wxLogMessage(wxS("Error: Unable to read font file '") + fileName + wxS("'."));
    return;
  }
  // Extract number of tables
  ttfFile.SeekI(0, wxFromCurrent);
  int id = ReadInt(&ttfFile);
  if (id != 0x00010000 && id != 0x4f54544f)
  {
    wxLogError(wxS("Error: File '") + fileName + wxS("' is not a valid font file."));
    return;
  }
  short nb = ReadShort(&ttfFile);
  ttfFile.SeekI(6, wxFromCurrent);
  // Seek OS/2 table
  bool found = false;
  int offset = 0;
  int i;
  for (i = 0; i < nb; i++)
  {
    char buffer[4];
    ttfFile.Read(buffer,4);
    if (strncmp(buffer,"OS/2",4) == 0)
    {
      found = true;
      ttfFile.SeekI(4, wxFromCurrent);
      offset = ReadInt(&ttfFile);
      ttfFile.SeekI(4, wxFromCurrent);
    }
    else if (strncmp(buffer,"CFF ",4) == 0)
    {
      ttfFile.SeekI(4, wxFromCurrent);
      cffOffset = ReadInt(&ttfFile);
      cffLength = ReadInt(&ttfFile);
    }
    else
    {
      ttfFile.SeekI(12, wxFromCurrent);
    }
  }
  if (!found)
  {
    return;
  }
  ttfFile.SeekI(offset, wxFromStart);
  // Extract fsType flags
  ttfFile.SeekI(8, wxFromCurrent);
  short fsType = ReadShort(&ttfFile);
  bool rl = (fsType & 0x02) != 0;
  bool pp = (fsType & 0x04) != 0;
  bool e  = (fsType & 0x08) != 0;
  bool ns = (fsType & 0x0100) != 0;
  bool eb = (fsType & 0x0200) != 0;
  embeddingAllowed = !(rl && !pp && !e) && !eb;
  subsettingAllowed = !ns;
}

/// Create wxPdfDocument font metrics file
void
MakeFont::CreateFontMetricsFile(const wxString& xmlFileName, wxPdfFontData& font, bool includeGlyphInfo)
{
  wxXmlDocument fontMetrics;
  const wxPdfFontDescription& fd = font.GetDescription();

  wxXmlNode* rootNode = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("wxpdfdoc-font-metrics"));
  rootNode->AddAttribute(wxS("type"), font.GetType() /*wxS("TrueTypeUnicode")*/);
  wxXmlNode* node;
  wxXmlNode* textNode;

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("font-name"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), font.GetName());
  node->AddChild(textNode);
  rootNode->AddChild(node);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("encoding"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), font.GetEncoding());
  node->AddChild(textNode);
  rootNode->AddChild(node);

  wxXmlNode* descNode = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("description"));

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("ascent"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), wxString::Format(wxS("%d"), fd.GetAscent()));
  node->AddChild(textNode);
  descNode->AddChild(node);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("descent"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), wxString::Format(wxS("%d"), fd.GetDescent()));
  node->AddChild(textNode);
  descNode->AddChild(node);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("cap-height"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), wxString::Format(wxS("%d"), fd.GetCapHeight()));
  node->AddChild(textNode);
  descNode->AddChild(node);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("flags"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), wxString::Format(wxS("%d"), fd.GetFlags()));
  node->AddChild(textNode);
  descNode->AddChild(node);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("font-bbox"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), fd.GetFontBBox());
  node->AddChild(textNode);
  descNode->AddChild(node);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("italic-angle"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), wxString::Format(wxS("%d"), fd.GetItalicAngle()));
  node->AddChild(textNode);
  descNode->AddChild(node);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("stem-v"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), wxString::Format(wxS("%d"), fd.GetStemV()));
  node->AddChild(textNode);
  descNode->AddChild(node);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("missing-width"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), wxString::Format(wxS("%d"), fd.GetMissingWidth()));
  node->AddChild(textNode);
  descNode->AddChild(node);
  
  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("x-height"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), wxString::Format(wxS("%d"), fd.GetXHeight()));
  node->AddChild(textNode);
  descNode->AddChild(node);
  
  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("underline-position"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), wxString::Format(wxS("%d"), fd.GetUnderlinePosition()));
  node->AddChild(textNode);
  descNode->AddChild(node);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("underline-thickness"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), wxString::Format(wxS("%d"), fd.GetUnderlineThickness()));
  node->AddChild(textNode);
  descNode->AddChild(node);

  rootNode->AddChild(descNode);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("diff"));
  textNode = new wxXmlNode(wxXML_TEXT_NODE, wxS("text"), font.GetDiffs());
  node->AddChild(textNode);
  rootNode->AddChild(node);

  node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("file"));
  node->AddAttribute(wxS("name"), font.GetFontFile());
  wxString fontType = font.GetType();
  if (fontType == wxS("TrueTypeUnicode") || fontType == wxS("OpenTypeUnicode"))
  {
    // TrueTypeUnicode (name,ctg,originalsize)
    node->AddAttribute(wxS("ctg"), font.GetCtgFile());
    node->AddAttribute(wxS("originalsize"), wxString::Format(wxS("%d"), font.GetSize1()));
  }
  else if (fontType == wxS("TrueType"))
  {
    // Truetype (name, originalsize)
    node->AddAttribute(wxS("originalsize"), wxString::Format(wxS("%d"), font.GetSize1()));
  }
  else if  (fontType == wxS("Type1"))
  {
    // Type1 (name, size1, size2)
    node->AddAttribute(wxS("size1"), wxString::Format(wxS("%d"), font.GetSize1()));
    if (font.HasSize2())
    {
      node->AddAttribute(wxS("size2"), wxString::Format(wxS("%d"), font.GetSize2()));
    }
  }
  rootNode->AddChild(node);

  wxXmlNode* widthsNode = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("widths"));
  if (includeGlyphInfo)
  {
    widthsNode->AddAttribute(wxS("subsetting"), wxS("enabled"));
  }

  wxPdfGlyphWidthMap* widths = (wxPdfGlyphWidthMap*) font.GetGlyphWidthMap();
  wxPdfChar2GlyphMap* glyphs = (wxPdfChar2GlyphMap*) font.GetChar2GlyphMap();
  wxPdfSortedArrayInt idList(CompareInts);
  wxPdfGlyphWidthMap::iterator iter = widths->begin();
  for (iter = widths->begin(); iter != widths->end(); iter++)
  {
    idList.Add(iter->first);
  }
  size_t j;
  for (j = 0; j < idList.GetCount(); j++)
  {
    node = new wxXmlNode(wxXML_ELEMENT_NODE, wxS("char"));
    node->AddAttribute(wxS("id"), wxString::Format(wxS("%d"), idList[j]));
    if (includeGlyphInfo)
    {
      int glyph = (*glyphs)[idList[j]];
      node->AddAttribute(wxS("gn"), wxString::Format(wxS("%d"), glyph));
    }
    node->AddAttribute(wxS("width"), wxString::Format(wxS("%d"), (*widths)[idList[j]]));
    widthsNode->AddChild(node);
  }
  rootNode->AddChild(widthsNode);

  wxXmlDocument voltData;
  wxXmlNode* voltRoot = NULL;
  if (!m_volt.IsEmpty())
  {
    wxFileSystem fs;
    // Open volt data XML file
    wxFSFile* xmlVoltData = fs.OpenFile(wxFileSystem::FileNameToURL(m_volt));
    if (xmlVoltData != NULL)
    {
      // Load the XML file
      bool loaded = voltData.Load(*(xmlVoltData->GetStream()));
      delete xmlVoltData;
      if (loaded)
      {
        if (voltData.IsOk() && voltData.GetRoot()->GetName().IsSameAs(wxS("volt")))
        {
          voltRoot = voltData.GetRoot();
          rootNode->AddChild(voltRoot);
        }
      }
    }
  }

  fontMetrics.SetRoot(rootNode);
  fontMetrics.Save(xmlFileName);
  if (voltRoot != NULL)
  {
    rootNode->RemoveChild(voltRoot);
  }
}

/// Make wxPdfDocument font metrics file based on AFM file
bool
MakeFont::MakeFontAFM(const wxString& fontFileName, const wxString& afmFileName,
                      const wxString& encoding, const wxString& type, const wxString& patch)
{
  bool valid = false;
  wxString fontType = type;
  wxFileName fileName(fontFileName);
  // Find font type
  if (fontFileName.Length() > 0)
  {
    wxString ext = fileName.GetExt();
    if (ext == wxS("ttf"))
    {
      fontType = wxS("TrueType");
    }
    else if (ext == wxS("pfb"))
    {
      fontType = wxS("Type1");
    }
    else
    {
      wxLogMessage(wxS("Warning: Unrecognized font file extension (") + ext + wxS("), ") + fontType + wxS(" font assumed."));
    }
  }

  bool embeddingAllowed = true;
  bool subsettingAllowed = true;
  if (fontFileName.Length() > 0)
  {
    int cffOffset, cffLength;
    if (fontType == wxS("TrueType"))
    {
      CheckTTF(fontFileName, embeddingAllowed, subsettingAllowed, cffOffset, cffLength);
    }
  }

  wxPdfFontData* afmFont;
  if (fontType == wxS("TrueType"))
  {
    afmFont = new wxPdfFontDataTrueType();
  }
  else
  {
    afmFont = new wxPdfFontDataType1();
  }
  // Initialize font description
  wxPdfFontDescription fd;
  fd.SetAscent(1000);
  fd.SetDescent(-200);
  fd.SetItalicAngle(0);
  fd.SetStemV(70);
  fd.SetMissingWidth(600);
  fd.SetUnderlinePosition(-100);
  fd.SetUnderlineThickness(50);
  
  bool hasCapHeight = false;
  bool hasXCapHeight = false;
  bool hasXHeight = false;
  bool hasFontBBox = false;
  bool hasStemV = false;
  bool hasMissingWidth = false;
  int flags = 0;

  CTGMap cc2gn;
  CTGMap cc2gnPatch;
  if (encoding.Length() > 0)
  {
    afmFont->SetEncoding(encoding);
    valid = ReadMap(encoding, cc2gn);
    if (!valid)
    {
      delete afmFont;
      return false;
    }
    if (patch.Length() > 0)
    {
      valid = ReadMap(patch, cc2gnPatch);
      if (valid)
      {
        CTGMap::iterator patchIter;
        for (patchIter = cc2gnPatch.begin(); patchIter != cc2gnPatch.end(); patchIter++)
        {
          cc2gn[patchIter->first] = patchIter->second;
        }
      }
      else
      {
        wxLogMessage(wxS("Warning: Unable to read patch file '") + patch + wxS("'."));
      }
    }
  }

  // Read a font metric file
  wxFileInputStream afmFile(afmFileName);
  if (!afmFile.Ok())
  {
    wxLogMessage(wxS("Error: Unable to read AFM file '") + afmFileName + wxS("'."));
    delete afmFont;
    return false;
  }
  wxTextInputStream text(afmFile);

  FixGlyphMap fix;

  fix[wxS("Edot")] = wxS("Edotaccent");
  fix[wxS("edot")] = wxS("edotaccent");
  fix[wxS("Idot")] = wxS("Idotaccent");
  fix[wxS("Zdot")] = wxS("Zdotaccent");
  fix[wxS("zdot")] = wxS("zdotaccent");
  fix[wxS("Odblacute")] = wxS("Ohungarumlaut");
  fix[wxS("odblacute")] = wxS("ohungarumlaut");
  fix[wxS("Udblacute")] = wxS("Uhungarumlaut");
  fix[wxS("udblacute")] = wxS("uhungarumlaut");
  fix[wxS("Gcedilla")] = wxS("Gcommaaccent");
  fix[wxS("gcedilla")] = wxS("gcommaaccent");
  fix[wxS("Kcedilla")] = wxS("Kcommaaccent");
  fix[wxS("kcedilla")] = wxS("kcommaaccent");
  fix[wxS("Lcedilla")] = wxS("Lcommaaccent");
  fix[wxS("lcedilla")] = wxS("lcommaaccent");
  fix[wxS("Ncedilla")] = wxS("Ncommaaccent");
  fix[wxS("ncedilla")] = wxS("ncommaaccent");
  fix[wxS("Rcedilla")] = wxS("Rcommaaccent");
  fix[wxS("rcedilla")] = wxS("rcommaaccent");
  fix[wxS("Scedilla")] = wxS("Scommaaccent");
  fix[wxS("scedilla")] = wxS("scommaaccent");
  fix[wxS("Tcedilla")] = wxS("Tcommaaccent");
  fix[wxS("tcedilla")] = wxS("tcommaaccent");
  fix[wxS("Dslash")] = wxS("Dcroat");
  fix[wxS("dslash")] = wxS("dcroat");
  fix[wxS("Dmacron")] = wxS("Dcroat");
  fix[wxS("dmacron")] = wxS("dcroat");
  fix[wxS("combininggraveaccent")] = wxS("gravecomb");
  fix[wxS("combininghookabove")] = wxS("hookabovecomb");
  fix[wxS("combiningtildeaccent")] = wxS("tildecomb");
  fix[wxS("combiningacuteaccent")] = wxS("acutecomb");
  fix[wxS("combiningdotbelow")] = wxS("dotbelowcomb");
  fix[wxS("dongsign")] = wxS("dong");

  // Read the AFM font metric file
  wxPdfGlyphWidthMap* widths = new wxPdfGlyphWidthMap();
  wxPdfChar2GlyphMap* glyphs = new wxPdfChar2GlyphMap();
  long cc;
  if (!cc2gn.empty())
  {
    for (cc = 0; cc <= 255; cc++)
    {
      (*widths)[cc] = 0xFFFF;
      (*glyphs)[cc] = 0;
    }
  }

  wxString line;
  wxString charcode, glyphname;
  wxString code, param, dummy, glyph;
  wxString token, tokenBoxHeight;
  long nParam, incWidth = -1, incGlyph = 0;
  long width, boxHeight, glyphNumber;
  wxString weight;
  bool hasGlyphNumbers = false;
  while (!afmFile.Eof())
  {
    line = text.ReadLine();
    line.Trim();

    wxStringTokenizer tkz(line, wxS(" "));
    int count = (int) tkz.CountTokens();
    if (count < 2) continue;
    code  = tkz.GetNextToken(); // 0
    param = tkz.GetNextToken(); // 1
    if (code == wxS("C"))
    {
      width = -1;
      glyphNumber = 0;
      boxHeight = 0;
      tokenBoxHeight = wxEmptyString;
      // Character metrics
      param.ToLong(&cc);
      dummy = tkz.GetNextToken(); // 2
      while (tkz.HasMoreTokens())
      {
        token = tkz.GetNextToken();
        if (token == wxS("WX")) // Character width
        {
          param = tkz.GetNextToken(); // Width
          param.ToLong(&width);
          dummy = tkz.GetNextToken(); // Semicolon
        }
        else if (token == wxS("N")) // Glyph name
        {
          glyphname = tkz.GetNextToken(); // Glyph name
          dummy = tkz.GetNextToken(); // Semicolon
        }
        else if (token == wxS("G")) // Glyph number
        {
          hasGlyphNumbers = true;
          param = tkz.GetNextToken(); // Number
          param.ToLong(&glyphNumber);
          dummy = tkz.GetNextToken(); // Semicolon
        }
        else if (token == wxS("B")) // Character bounding box
        {
          dummy = tkz.GetNextToken(); // x left
          dummy = tkz.GetNextToken(); // y bottom
          dummy = tkz.GetNextToken(); // x right
          tokenBoxHeight = tkz.GetNextToken(); // y top
          tokenBoxHeight.ToLong(&boxHeight);
          dummy = tkz.GetNextToken(); // Semicolon
        }
        else
        {
          while (tkz.HasMoreTokens() && tkz.GetNextToken() != wxS(";"));
        }
      }

      if (glyphname.Right(4) == wxS("20AC"))
      {
        glyphname = wxS("Euro");
      }
      if (glyphname == wxS("increment"))
      {
        incWidth = width;
        if (hasGlyphNumbers)
        {
          incGlyph = glyphNumber;
        }
      }
      if (fix.find(glyphname) != fix.end())
      {
        //Fix incorrect glyph name
        for (int i = 0; i <= 255; i++)
        {
          if (cc2gn[i] == fix[glyphname])
          {
            cc2gn[i] = glyphname;
          }
        }
      }
      if (cc2gn.empty())
      {
        // Symbolic font: use built-in encoding
        (*widths)[cc] = width;
        if (hasGlyphNumbers)
        {
          (*glyphs)[cc] = glyphNumber;
        }
        if (!hasCapHeight && !hasXCapHeight && cc == 'X' && tokenBoxHeight != wxEmptyString)
        {
          hasXCapHeight = true;
          fd.SetCapHeight(boxHeight);
        }
        if (!hasXHeight && cc == 'x' && tokenBoxHeight != wxEmptyString)
        {
          hasXHeight = true;
          fd.SetXHeight(boxHeight);
        }
      }
      else
      {
        // Search glyphname in cc2gn
        bool found = false;
        for (int i = 0; i <= 255; i++)
        {
          if (cc2gn[i] == glyphname)
          {
            found = true;
            (*widths)[i] = width;
            if (hasGlyphNumbers)
            {
              (*glyphs)[i] = glyphNumber;
            }
          }
        }
        if (!found)
        {
          wxLogMessage(wxS("Warning: character '") + glyphname + wxS("' is undefined."));
        }
        if (!hasCapHeight && !hasXCapHeight && glyphname == wxS("X") && tokenBoxHeight != wxEmptyString)
        {
          hasXCapHeight = true;
          fd.SetCapHeight(boxHeight);
        }
        if (!hasXHeight && glyphname == wxS("x") && tokenBoxHeight != wxEmptyString)
        {
          hasXHeight = true;
          fd.SetXHeight(boxHeight);
        }
      }
      if (!hasMissingWidth && glyphname == wxS(".notdef"))
      {
        hasMissingWidth = true;
        fd.SetMissingWidth(width);
      }
    }
    else if (code == wxS("FontName"))
    {
      afmFont->SetName(param);
    }
    else if (code == wxS("Weight"))
    {
      wxString weight = param.Lower();
      if (!hasStemV && (weight == wxS("black") || weight == wxS("bold")))
      {
        fd.SetStemV(120);
      }
    }
    else if (code == wxS("ItalicAngle"))
    {
      double italic;
      param.ToDouble(&italic);
      int italicAngle = int(italic);
      fd.SetItalicAngle(italicAngle);
      if (italicAngle > 0)
      {
        flags += 1 << 6;
      }
    }
    else if (code == wxS("Ascender"))
    {
      long ascent;
      param.ToLong(&ascent);
      fd.SetAscent(ascent);
    }
    else if (code == wxS("Descender"))
    {
      param.ToLong(&nParam);
      fd.SetDescent(nParam);
    }
    else if (code == wxS("UnderlineThickness"))
    {
      param.ToLong(&nParam);
      fd.SetUnderlineThickness(nParam);
    }
    else if (code == wxS("UnderlinePosition"))
    {
      param.ToLong(&nParam);
      fd.SetUnderlinePosition(nParam);
    }
    else if (code == wxS("IsFixedPitch"))
    {
      if (param == wxS("true"))
      {
        flags += 1 << 0;
      }
    }
    else if (code == wxS("FontBBox"))
    {
      hasFontBBox = true;
      wxString bbox2 = tkz.GetNextToken();
      wxString bbox3 = tkz.GetNextToken();
      wxString bbox4 = tkz.GetNextToken();
      wxString bBox = wxS("[") + param + wxS(" ") + bbox2 + wxS(" ") + bbox3 + wxS(" ") + bbox4 + wxS("]");
      fd.SetFontBBox(bBox);
    }
    else if (code == wxS("CapHeight"))
    {
      hasCapHeight = true;
      long capHeight;
      param.ToLong(&capHeight);
      fd.SetCapHeight(capHeight);
    }
    else if (code == wxS("StdVW"))
    {
      hasStemV = true;
      long stemV;
      param.ToLong(&stemV);
      fd.SetStemV(stemV);
    }
  }

  for (cc = 0; cc <= 255; cc++)
  {
    if ((*widths)[cc] == 0xFFFF)
    {
      if (cc2gn[cc] == wxS("Delta") && incWidth >= 0)
      {
        (*widths)[cc] = incWidth;
        if (hasGlyphNumbers)
        {
          (*glyphs)[cc] = incGlyph;
        }
      }
      else
      {
        (*widths)[cc] = fd.GetMissingWidth();
        if (hasGlyphNumbers)
        {
          (*glyphs)[cc] = 0;
        }
        wxLogMessage(wxS("Warning: character '") + cc2gn[cc] + wxS("' is missing."));
      }
    }
  }

  wxString diffs;
  if (encoding.Length() > 0)
  {
    // Build differences from reference encoding
    CTGMap cc2gnRef;
    valid = ReadMap(wxS("cp1252"), cc2gnRef);
    diffs = wxS("");
    int last = 0;
    for (int i = 32; i <= 255; i++)
    {
      if (cc2gn[i] != cc2gnRef[i])
      {
        if (i != last+1)
        {
          diffs += wxString::Format(wxS("%d "), i);
        }
        last = i;
        diffs = diffs + wxS("/") + cc2gn[i] + wxS(" ");
      }
    }
  }
  afmFont->SetDiffs(diffs);  
  
  if (cc2gn.empty())
  {
    flags += 1 << 2;
  }
  else
  {
    flags += 1 << 5;
  }
  fd.SetFlags(flags);
  if (!hasCapHeight && !hasXCapHeight)
  {
    fd.SetCapHeight(fd.GetAscent());
  }
  if (!hasFontBBox)
  {
    wxString fbb = wxString::Format(wxS("[0 %d 1000 %d]"), fd.GetDescent()-100, fd.GetAscent()+100);
    fd.SetFontBBox(fbb);
  }

  afmFont->SetGlyphWidthMap(widths);
  afmFont->SetChar2GlyphMap(glyphs);

  afmFont->SetDescription(fd);

  wxString baseName = fileName.GetName();
  if (embeddingAllowed)
  {
    if (fontType == wxS("TrueType"))
    {
      wxFileInputStream ttfFile(fontFileName);
      if (!ttfFile.Ok())
      {
        wxLogMessage(wxS("Error: Unable to read font file '") + fontFileName + wxS("'."));
        delete afmFont;
        return false;
      }
      size_t len = ttfFile.GetLength();
      wxFileOutputStream outFontFile(m_outputDir + baseName + wxS(".z"));
      wxZlibOutputStream zOutFontFile(outFontFile);
      zOutFontFile.Write(ttfFile);
      zOutFontFile.Close();
      afmFont->SetFontFile(baseName + wxS(".z"));
      afmFont->SetSize1(len);
      wxLogMessage(wxS("Font file compressed (") + baseName + wxS(".z)."));
    }
    else
    {
      wxFileInputStream pfbFile(fontFileName);
      if (!pfbFile.Ok())
      {
        wxLogMessage(wxS("Error: Unable to read font file '") + fontFileName + wxS("'."));
        delete afmFont;
        return false;
      }
      size_t len = pfbFile.GetLength();
      // Find first two sections and discard third one
      unsigned char* buffer = new unsigned char[len];
      unsigned char* buf1 = buffer;
      unsigned char* buf2;
      pfbFile.Read(buffer, len);
      unsigned char first = buffer[0];
      if (first == 128)
      {
        // Strip first binary header
        buf1 += 6;
        len -= 6;
      }
      int* f = makeFail("eexec",5);
      int size1 = (int) findString((char*) buf1, len, "eexec", 5, f);
      delete [] f;

      int size2 = -1;
      if (size1 >= 0)
      {
        size1 += 6;
        unsigned char second = buf1[size1];
        buf2 = buf1 + size1;
        len -= size1;
        if (first == 128 && second == 128)
        {
          // Strip second binary header
          buf2 += 6;
          len -= 6;
        }
        f = makeFail("00000000",8);
        size2 = (int) findString((char*) buf2, len, "00000000", 8, f);
        delete [] f;
        if (size2 >= 0)
        {
          wxFileOutputStream outFontFile(m_outputDir + baseName + wxS(".z"));
          wxZlibOutputStream zOutFontFile(outFontFile);
          zOutFontFile.Write(buf1, size1);
          zOutFontFile.Write(buf2, size2);
          afmFont->SetFontFile(baseName + wxS(".z"));
          afmFont->SetSize1(size1);
          afmFont->SetSize2(size2);
        }
      }
      delete [] buffer;
      if (size1 < 0 || size2 < 0)
      {
        wxLogMessage(wxS("Warning: Font file does not seem to be valid Type1, font embedding not possible."));
      }
    }
  }
  else
  {
    if (fontFileName.Length() > 0)
    {
      wxLogMessage(wxS("Warning: Font license does not allow embedding."));
    }
    else
    {
      wxLogMessage(wxS("Warning: Font file name missing, font embedding not possible."));
    }
  }

  // Create XML file
  CreateFontMetricsFile(m_outputDir + baseName + wxS(".xml"), *afmFont, hasGlyphNumbers);
  wxLogMessage(wxS("Font definition file generated (") + baseName + wxS(".xml)."));

  //delete widths;
  delete afmFont;
  return true;
}

#if wxUSE_UNICODE

void
MakeFont::WriteStreamBuffer(wxOutputStream& stream, const char* buffer)
{
  size_t buflen = strlen(buffer);
  stream.Write(buffer, buflen);
}

void
MakeFont::WriteToUnicode(GlyphList& glyphs, wxMemoryOutputStream& toUnicode)
{
  WriteStreamBuffer(toUnicode, "/CIDInit /ProcSet findresource begin\n");
  WriteStreamBuffer(toUnicode, "12 dict begin\n");
  WriteStreamBuffer(toUnicode, "begincmap\n");
  WriteStreamBuffer(toUnicode, "/CIDSystemInfo\n");
  WriteStreamBuffer(toUnicode, "<< /Registry (Adobe)\n");
  WriteStreamBuffer(toUnicode, "/Ordering (UCS)\n");
  WriteStreamBuffer(toUnicode, "/Supplement 0\n");
  WriteStreamBuffer(toUnicode, ">> def\n");
  WriteStreamBuffer(toUnicode, "/CMapName /Adobe-Identity-UCS def\n");
  WriteStreamBuffer(toUnicode, "/CMapType 2 def\n");
  WriteStreamBuffer(toUnicode, "1 begincodespacerange\n");
  WriteStreamBuffer(toUnicode, "<0000><FFFF>\n");
  WriteStreamBuffer(toUnicode, "endcodespacerange\n");
  int size = 0;
  size_t k;
  size_t numGlyphs = glyphs.GetCount();
  for (k = 0; k < numGlyphs; ++k)
  {
    if (size == 0)
    {
      if (k != 0)
      {
        WriteStreamBuffer(toUnicode, "endbfrange\n");
      }
      size = (numGlyphs-k > 100) ? 100 : numGlyphs - k;
      wxString sizeStr = wxString::Format(wxS("%d"), size);
      WriteStreamBuffer(toUnicode, sizeStr.ToAscii());
      WriteStreamBuffer(toUnicode, " beginbfrange\n");
    }
    size--;
    GlyphListEntry* entry = glyphs[k];
    wxString fromTo = wxString::Format(wxS("<%04x>"), entry->m_gid);
    wxString uniChr = wxString::Format(wxS("<%04x>"), entry->m_uid);
    WriteStreamBuffer(toUnicode, fromTo.ToAscii());
    WriteStreamBuffer(toUnicode, fromTo.ToAscii());
    WriteStreamBuffer(toUnicode, uniChr.ToAscii());
    WriteStreamBuffer(toUnicode, "\n");
  }
  WriteStreamBuffer(toUnicode, "endbfrange\n");
  WriteStreamBuffer(toUnicode, "endcmap\n");
  WriteStreamBuffer(toUnicode, "CMapName currentdict /CMap defineresource pop\n");
  WriteStreamBuffer(toUnicode, "end end\n");
}

/// Make wxPdfDocument font metrics file based on UFM file
bool
MakeFont::MakeFontUFM(const wxString& fontFileName,
                      const wxString& ufmFileName,
                      const wxString& type,
                      const wxString& volt)
{
  bool hasVolt = !volt.IsEmpty();
  int paBase = 0xe000;
  bool cff = false;
  GlyphList glyphList(CompareGlyphListEntries);
  static size_t CC2GNSIZE = 131072; // 2*64kB
  wxFileName fileName(ufmFileName);
  bool embeddingAllowed = false;
  bool subsettingAllowed = false;
  int cffOffset = -1;
  int cffLength = 0;
  if (fontFileName.Length() > 0)
  {
    CheckTTF(fontFileName, embeddingAllowed, subsettingAllowed, cffOffset, cffLength);
    cff = cffOffset > 0;
  }
  else
  {
    cff = (type == wxS("OpenType"));
  }

  wxPdfFontData* ufmFont;
  if (cff)
  {
    ufmFont = new wxPdfFontDataOpenTypeUnicode();
  }
  else
  {
    ufmFont = new wxPdfFontDataTrueTypeUnicode();
  }

  // Initialize font description
  wxPdfFontDescription fd;
  fd.SetAscent(1000);
  fd.SetDescent(-200);
  fd.SetItalicAngle(0);
  fd.SetStemV(70);
  fd.SetMissingWidth(600);
  bool hasCapHeight = false;
  bool hasXCapHeight = false;
  bool hasXHeight = false;
  bool hasFontBBox = false;
  bool hasStemV = false;
  bool hasMissingWidth = false;
  int flags = 0;

  wxFileInputStream ufmFile(ufmFileName);
  if (!ufmFile.Ok())
  {
    wxLogMessage(wxS("Error: Unable to read UFM file '") + ufmFileName + wxS("'."));
    return false;
  }
  wxTextInputStream text(ufmFile);

  // Prepare empty CIDToGIDMap
  unsigned char* cc2gn = new unsigned char[131072];
  size_t j;
  for (j = 0; j < CC2GNSIZE; j++)
  {
    cc2gn[j] = '\0';
  }
  
  // Read the UFM font metric file
  wxPdfGlyphWidthMap* widths = new wxPdfGlyphWidthMap();
  wxPdfChar2GlyphMap* glyphs = new wxPdfChar2GlyphMap();

  wxString line;
  wxString charcode, glyphname;
  wxString code, param, dummy, glyph;
  wxString token, tokenBoxHeight;
  long nParam, width, glyphNumber, boxHeight;
  wxString weight;
  while (!ufmFile.Eof())
  {
    line = text.ReadLine();
    line.Trim();

    wxStringTokenizer tkz(line, wxS(" "));
    int count = (int) tkz.CountTokens();
    if (count < 2) continue;
    code  = tkz.GetNextToken(); // 0
    param = tkz.GetNextToken(); // 1
    if (code == wxS("U"))
    {
      long cc;
      param.ToLong(&cc); // Character code
      width = -1;
      glyphNumber = 0;
      boxHeight = 0;
      tokenBoxHeight = wxEmptyString;
      dummy = tkz.GetNextToken(); // Semicolon
      while (tkz.HasMoreTokens())
      {
        token = tkz.GetNextToken();
        if (token == wxS("WX")) // Character width
        {
          param = tkz.GetNextToken(); // Width
          param.ToLong(&width);
          dummy = tkz.GetNextToken(); // Semicolon
        }
        else if (token == wxS("N")) // Glyph name
        {
          glyphname = tkz.GetNextToken(); // Glyph name
          dummy = tkz.GetNextToken(); // Semicolon
        }
        else if (token == wxS("G")) // Glyph number
        {
          param = tkz.GetNextToken(); // Number
          param.ToLong(&glyphNumber);
          dummy = tkz.GetNextToken(); // Semicolon
        }
        else if (token == wxS("B")) // Character bounding box
        {
          dummy = tkz.GetNextToken(); // x left
          dummy = tkz.GetNextToken(); // y bottom
          dummy = tkz.GetNextToken(); // x right
          tokenBoxHeight = tkz.GetNextToken(); // y top
          tokenBoxHeight.ToLong(&boxHeight);
          dummy = tkz.GetNextToken(); // Semicolon
        }
        else
        {
          while (tkz.HasMoreTokens() && tkz.GetNextToken() != wxS(";"));
        }
      }
      if (cc != -1 || (hasVolt && glyphNumber > 0))
      {
        if (!hasCapHeight && !hasXCapHeight && cc == 'X' && tokenBoxHeight != wxEmptyString)
        {
          hasXCapHeight = true;
          fd.SetCapHeight(boxHeight);
        }
        if (!hasXHeight && cc == 'x' && tokenBoxHeight != wxEmptyString)
        {
          hasXHeight = true;
          fd.SetXHeight(boxHeight);
        }

        if (cc == -1)
        {
          cc = paBase++;
        }
        (*widths)[cc] = width;
        (*glyphs)[cc] = glyphNumber;
        // Set GID
        if (cc >= 0 && cc < 0xFFFF)
        {
          cc2gn[2*cc]   = (glyphNumber >> 8) & 0xFF;
          cc2gn[2*cc+1] =  glyphNumber       & 0xFF;
        }
        if (cff)
        {
          GlyphListEntry* glEntry = new GlyphListEntry();
          glEntry->m_gid = glyphNumber;
          glEntry->m_uid = cc;
          glyphList.Add(glEntry);
        }
      }
      if (!hasMissingWidth && glyphname == wxS(".notdef"))
      {
        hasMissingWidth = true;
        fd.SetMissingWidth(width);
      }
    }
    else if (code == wxS("FontName"))
    {
      ufmFont->SetName(param);
    }
    else if (code == wxS("Weight"))
    {
      wxString weight = param.Lower();
      if (!hasStemV && (weight == wxS("black") || weight == wxS("bold")))
      {
        fd.SetStemV(120);
      }
    }
    else if (code == wxS("ItalicAngle"))
    {
      double italic;
      param.ToDouble(&italic);
      int italicAngle = int(italic);
      fd.SetItalicAngle(italicAngle);
      if (italicAngle > 0)
      {
        flags += 1 << 6;
      }
    }
    else if (code == wxS("Ascender"))
    {
      long ascent;
      param.ToLong(&ascent);
      fd.SetAscent(ascent);
    }
    else if (code == wxS("Descender"))
    {
      param.ToLong(&nParam);
      fd.SetDescent(nParam);
    }
    else if (code == wxS("UnderlineThickness"))
    {
      param.ToLong(&nParam);
      fd.SetUnderlineThickness(nParam);
    }
    else if (code == wxS("UnderlinePosition"))
    {
      param.ToLong(&nParam);
      fd.SetUnderlinePosition(nParam);
    }
    else if (code == wxS("IsFixedPitch"))
    {
      if (param == wxS("true"))
      {
        flags += 1 << 0;
      }
    }
    else if (code == wxS("FontBBox"))
    {
      hasFontBBox = true;
      wxString bbox2 = tkz.GetNextToken();
      wxString bbox3 = tkz.GetNextToken();
      wxString bbox4 = tkz.GetNextToken();
      wxString bBox = wxS("[") + param + wxS(" ") + bbox2 + wxS(" ") + bbox3 + wxS(" ") + bbox4 + wxS("]");
      fd.SetFontBBox(bBox);
    }
    else if (code == wxS("CapHeight"))
    {
      hasCapHeight = true;
      long capHeight;
      param.ToLong(&capHeight);
      fd.SetCapHeight(capHeight);
    }
    else if (code == wxS("StdVW"))
    {
      hasStemV = true;
      long stemV;
      param.ToLong(&stemV);
      fd.SetStemV(stemV);
    }
  }
  flags += 1 << 5;
  fd.SetFlags(flags);
  if (!hasCapHeight && !hasXCapHeight)
  {
    fd.SetCapHeight(fd.GetAscent());
  }
  if (!hasFontBBox)
  {
    hasFontBBox = true;
    wxString fbb = wxString::Format(wxS("[0 %d 1000 %d]"), fd.GetDescent()-100, fd.GetAscent()+100);
    fd.SetFontBBox(fbb);
  }

  ufmFont->SetGlyphWidthMap(widths);
  ufmFont->SetChar2GlyphMap(glyphs);

  ufmFont->SetDescription(fd);

  wxString baseName = fileName.GetName();
  if (embeddingAllowed)
  {
    wxFileInputStream fontFile(fontFileName);
    if (!fontFile.Ok())
    {
      wxLogMessage(wxS("Error: Unable to read font file '") + fontFileName + wxS("'."));
      delete [] cc2gn;
      return false;
    }
    size_t len = fontFile.GetLength();
    wxFileOutputStream outFontFile(m_outputDir + baseName + wxS(".z"));
    wxZlibOutputStream zOutFontFile(outFontFile);
    if (cff)
    {
      char* buffer = new char[cffLength];
      fontFile.SeekI(cffOffset);
      fontFile.Read(buffer, cffLength);
      zOutFontFile.Write(buffer, cffLength);
      delete [] buffer;
    }
    else
    {
      zOutFontFile.Write(fontFile);
    }
    zOutFontFile.Close();
    ufmFont->SetFontFile(baseName + wxS(".z"));
    ufmFont->SetSize1(len);
    wxLogMessage(wxS("Font file compressed (") + baseName + wxS(".z)."));
  }
  else
  {
    if (fontFileName.Length() > 0)
    {
      wxLogMessage(wxS("Warning: Font license does not allow embedding."));
    }
    else
    {
      wxLogMessage(wxS("Warning: Font file name missing, font embedding not possible."));
    }
  }

  if (embeddingAllowed)
  {
    // Create CID to GID map file
    wxFileOutputStream outCtgFile(m_outputDir + baseName + wxS(".ctg.z"));
    wxZlibOutputStream zOutCtgFile(outCtgFile);
    if (cff)
    {
      wxMemoryOutputStream toUnicode;
      WriteToUnicode(glyphList, toUnicode);
      wxMemoryInputStream inUnicode(toUnicode);
      zOutCtgFile.Write(inUnicode);
    }
    else
    {
#if 0
// Debug
      size_t j;
      for (j = 0; j < CC2GNSIZE; j+=2)
      {
        wxLogMessage(wxS("C %ld G %ld"), j/2, cc2gn[j] * 256 + cc2gn[j+1]);
      }
#endif
      zOutCtgFile.Write(cc2gn, CC2GNSIZE);
    }
    zOutCtgFile.Close();
    ufmFont->SetCtgFile(baseName + wxS(".ctg.z"));
    wxLogMessage(wxS("CIDToGIDMap created and compressed (") + baseName + wxS(".ctg.z)"));
  }

  // Create XML file
  CreateFontMetricsFile(m_outputDir + baseName + wxS(".xml"), *ufmFont, cff);
  wxLogMessage(wxS("Font definition file generated (") + baseName + wxS(".xml)."));

  if (cff)
  {
    WX_CLEAR_ARRAY(glyphList);
  }
  //delete widths;
  delete [] cc2gn;
  delete ufmFont;

  return true;
}

/// Make wxPdfDocument font metrics file immediately from font file
bool
MakeFont::MakeFontImmediate(const wxString& fontFileName)
{
  wxFileName fileName(fontFileName);

  wxFileInputStream fontStream(fontFileName);
  if (!fontStream.Ok())
  {
    wxLogMessage(wxS("Error: Unable to read font file '") + fontFileName + wxS("'."));
    return false;
  }
  size_t len = fontStream.GetLength();

  int cffOffset, cffLength;
  bool embeddingAllowed = false; 
  bool subsettingAllowed = false;
  CheckTTF(fontFileName, embeddingAllowed, subsettingAllowed, cffOffset, cffLength);
  bool cff = cffOffset > 0;

  wxPdfFontParserTrueType fontParser;
  wxPdfFontData* loadedFont = fontParser.IdentifyFont(fileName.GetFullName(), 0);
  if (loadedFont != NULL && fontParser.LoadFontData(loadedFont))
  {
    wxString baseName = fileName.GetName();
    if (loadedFont->EmbedSupported())
    {
      wxFileOutputStream outFontFile(m_outputDir + baseName + wxS(".z"));
      loadedFont->WriteFontData(&outFontFile);
      outFontFile.Close();
      wxLogMessage(wxS("Font file compressed (") + baseName + wxS(".z)"));
      wxFileOutputStream outCtgFile(m_outputDir + baseName + wxS(".ctg.z"));
      if (!cff)
      {
        static size_t CC2GNSIZE = 131072; // 2*64kB
        unsigned char* cc2gn = new unsigned char[131072];
        size_t j;
        for (j = 0; j < CC2GNSIZE; j++)
        {
          cc2gn[j] = '\0';
        }
        const wxPdfChar2GlyphMap* ctgMap = loadedFont->GetChar2GlyphMap();
        wxPdfChar2GlyphMap::const_iterator ccIter;
        for (ccIter = ctgMap->begin(); ccIter != ctgMap->end(); ccIter++)
        {
          wxUint32 cc = ccIter->first;
          wxUint32 gn = ccIter->second;
          if (cc < 0xFFFF)
          {
            cc2gn[2*cc]   = (gn >> 8) & 0xFF;
            cc2gn[2*cc+1] =  gn       & 0xFF;
          }
        }
        wxZlibOutputStream zOutCtgFile(outCtgFile);
        zOutCtgFile.Write(cc2gn, CC2GNSIZE);
      }
      else
      {
        loadedFont->WriteUnicodeMap(&outCtgFile);
      }
      outCtgFile.Close();
      wxLogMessage(wxS("CIDToGIDMap file compressed (") + baseName + wxS(".ctg.z)"));
      loadedFont->SetFontFile(baseName + wxS(".z"));
      loadedFont->SetSize1(len);
      loadedFont->SetCtgFile(baseName + wxS(".ctg.z"));
    }
    else
    {
      loadedFont->SetFontFile(wxS(""));
      loadedFont->SetCtgFile(wxS(""));
      wxLogMessage(wxS("Warning: Font license does not allow embedding."));
    }

    // Create XML file
    CreateFontMetricsFile(m_outputDir + baseName + wxS(".xml"), *loadedFont, cff);
    wxLogMessage(wxS("Font definition file generated (") + baseName + wxS(".xml)."));
    delete loadedFont;
  }

  return true;
}

#endif // wxUSE_UNICODE

static const wxCmdLineEntryDesc cmdLineDesc[] =
{
  { wxCMD_LINE_OPTION, "a", "afm",       "Adobe Font Metric file (AFM)",                    wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
#if wxUSE_UNICODE
  { wxCMD_LINE_OPTION, "u", "ufm",       "Unicode Font Metric file (UFM)",                  wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
  { wxCMD_LINE_SWITCH, "i", "immediate", "Extract font metrics from ttf/otf font file",     wxCMD_LINE_VAL_NONE,   wxCMD_LINE_PARAM_OPTIONAL },
  { wxCMD_LINE_OPTION, "f", "font",      "Font file (ttf, otf or pfb)",                     wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
  { wxCMD_LINE_OPTION, "v", "volt",      "Visual order layout table",                       wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
#else
  { wxCMD_LINE_OPTION, "f", "font",      "Font file (ttf or pfb)",                          wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
#endif
  { wxCMD_LINE_OPTION, "e", "enc",       "Character encoding of the font (for AFM only)",   wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
  { wxCMD_LINE_OPTION, "p", "patch",     "Patch file (for AFM only)",                       wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
  { wxCMD_LINE_OPTION, "t", "type",      "Font type (type = otf, ttf or t1, default: ttf)", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
  { wxCMD_LINE_OPTION, "o", "output",    "Path to output directory",                        wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
  { wxCMD_LINE_NONE }
};

bool
MakeFont::OnInit()
{
  // Set the font path and working directory
  wxFileName exePath = wxStandardPaths::Get().GetExecutablePath();
  wxString fontPath = exePath.GetPathWithSep() + wxS("../lib/fonts");
  wxString cwdPath  = exePath.GetPath();
  wxPdfFontManager::GetFontManager()->AddSearchPath(fontPath);
  wxSetWorkingDirectory(cwdPath);

  m_version = wxS("1.7.0 (January 2017)");
  bool valid = false;
  //gets the parameters from cmd line
  wxCmdLineParser parser (cmdLineDesc, argc, argv);
  wxString logo = wxS("wxPdfDocument MakeFont Utility Version ") + m_version + 
                  wxS("\n\nCreate wxPdfDocument font support files.\n") +
                  wxS("Please specify file extensions in ALL file name parameters.\n") +
                  wxS("Select exactly one method to provide font metrics.\n");
  parser.SetLogo(logo);
  if (parser.Parse() == 0)
  {
    bool hasAfm       = parser.Found(wxS("afm"),    &m_afmFile);
    bool hasUfm       = parser.Found(wxS("ufm"),    &m_ufmFile);
    bool hasImmediate = parser.Found(wxS("immediate"));
    bool hasFont      = parser.Found(wxS("font"),   &m_fontFile);
    bool hasEnc       = parser.Found(wxS("enc"),    &m_encoding);
    bool hasPatch     = parser.Found(wxS("patch"),  &m_patchFile);
    bool hasType      = parser.Found(wxS("type"),   &m_fontType);
    bool hasVolt      = parser.Found(wxS("volt"),   &m_volt);
    bool hasOutput    = parser.Found(wxS("output"), &m_outputDir);

#if wxUSE_UNICODE
    m_unicode = hasUfm || hasImmediate;
    m_immediate = hasImmediate;
#else
    hasUfm = false;
    m_unicode = false;
    m_immediate = false;
#endif
    if (!hasOutput)
    {
      m_outputDir = wxEmptyString;
    }
    valid = ( hasImmediate && !hasAfm && !hasUfm && !hasVolt) ||
            (!hasImmediate &&  hasAfm && !hasUfm && !hasVolt) ||
            (!hasImmediate && !hasAfm &&  hasUfm);
    if (valid)
    {
      if (hasAfm)
      {
        m_fontType = (hasType && m_fontType == wxS("t1")) ? wxS("Type1") : wxS("TrueType");
      }
      else if (hasUfm)
      {
        m_fontType = (hasType && m_fontType == wxS("otf")) ? wxS("OpenType") : wxS("TrueType");
      }
    }
    else
    {
      parser.Usage();
    }
  }
  return valid;
}

int
MakeFont::OnExit()
{
  return 0;
}

int
MakeFont::OnRun()
{
  bool valid;
  wxLogMessage(wxS("wxPdfDocument MakeFont Utility Version ") + m_version);
  wxLogMessage(wxS("*** Starting to create font support files for ..."));
  if (!m_outputDir.IsEmpty())
  {
    wxFileName outputDirName(m_outputDir, wxEmptyString);
    if (outputDirName.DirExists())
    {
      m_outputDir = outputDirName.GetPath(wxPATH_GET_VOLUME | wxPATH_GET_SEPARATOR);
    }
    else
    {
      wxLogMessage(wxS("*** Warning: Output directory doesn't exist: ") + m_outputDir);
      m_outputDir = wxEmptyString;
    }
  }
  if (!m_outputDir.IsEmpty())
  {
    wxLogMessage(wxS("  OutputDir: ") + m_outputDir);
  }
  else
  {
    wxLogMessage(wxS("  OutputDir: (current working directory)"));
  }
#if wxUSE_UNICODE
  if (m_unicode)
  {
    if (m_immediate)
    {
      wxLogMessage(wxS("  Metrics  : Immediately from font file"));
      wxLogMessage(wxS("  Font file: ") + m_fontFile);
      valid = MakeFontImmediate(m_fontFile);
    }
    else
    {
      wxLogMessage(wxS("  UFM file : ") + m_ufmFile);
      wxLogMessage(wxS("  Font file: ") + m_fontFile);
      if (!m_volt.IsEmpty())
      {
        wxLogMessage(wxS("  VOLT file: ") + m_volt);
      }
      valid = MakeFontUFM(m_fontFile, m_ufmFile, m_fontType, m_volt);
    }
  }
  else
#endif
  {
    wxLogMessage(wxS("  AFM file : ") + m_afmFile);
    wxLogMessage(wxS("  Font file: ") + m_fontFile);
    wxLogMessage(wxS("  Encoding : ") + m_encoding);
    wxLogMessage(wxS("  Font type: ") + m_fontType);
    valid = MakeFontAFM(m_fontFile, m_afmFile, m_encoding, m_patchFile);
  }
  wxLogMessage(wxS("*** wxPdfDocument MakeFont finished."));

  return 0;
}

IMPLEMENT_APP_CONSOLE(MakeFont)
