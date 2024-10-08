/*
 * (C) 2003-2006 Gabest
 * (C) 2006-2024 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

class CMediaTypeEx : public CMediaType
{
public:
	CMediaTypeEx();
	CMediaTypeEx(const CMediaType& mt) {
		CMediaType::operator = (mt);
	}
	CString ToString(IPin* pPin = nullptr);
	void Dump(std::list<CString>& sl);
	bool ValidateSubtitle();

	static CString GetVideoCodecName(const GUID& subtype, DWORD biCompression);
	static CString GetAudioCodecName(const GUID& subtype, WORD wFormatTag);

private:
	CString GetSubtitleCodecName(const GUID& subtype);
};

// get the name of a known media subtype, otherwise convert the GUID to a string
CStringW GetGUIDString(const GUID& guid);

// get the name of a known media subtype along with its GUID string
CStringW GetGUIDNameAndString(const GUID& guid);
