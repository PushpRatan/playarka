import { QRCodeSVG } from 'qrcode.react';

interface QRCodeProps {
  data: string;
  size?: number;
}

export function QRCode({ data, size = 200 }: QRCodeProps) {
  return (
    <div className="flex flex-col items-center justify-center">
      <div className="bg-white p-4 rounded border-2 border-gray-600/40">
        <QRCodeSVG value={data} size={size} />
      </div>
    </div>
  );
}

