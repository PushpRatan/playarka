interface PaymentScreenProps {
  paymentQr: string;
  amount: number | null;
  paymentStatus: string | null;
  onSimulatePayment?: () => void;
}

export function PaymentScreen({ paymentQr, amount, paymentStatus, onSimulatePayment }: PaymentScreenProps) {
  const handleQrClick = (e: React.MouseEvent) => {
    e.stopPropagation();
    if (onSimulatePayment && paymentStatus !== 'success') {
      console.log('💳 Simulating payment...');
      onSimulatePayment();
    }
  };

  return (
    <div className="flex flex-col items-center justify-center min-h-[calc(100vh-120px)] p-6">
      <div className="w-full max-w-md">
        <h2 className="text-3xl font-bold text-white mb-8 text-center">
          Payment Required
        </h2>

        {paymentStatus === 'success' ? (
          <div className="wood-panel border-2 border-green-500/50 rounded-lg p-6 text-center relative overflow-hidden">
            <div className="text-4xl mb-4">✅</div>
            <p className="text-lg font-semibold text-green-400">
              Payment Successful!
            </p>
            <p className="text-sm text-gray-400 mt-2">
              Game will start shortly...
            </p>
          </div>
        ) : (
          <>
            {amount && (
              <div className="wood-panel border-2 border-gray-600/40 rounded-lg p-4 mb-6 text-center relative overflow-hidden">
                <p className="text-sm text-gray-400">Amount to Pay</p>
                <p className="text-3xl font-bold text-white mt-2">
                  ₹{amount}
                </p>
              </div>
            )}

            <div className="wood-panel border-2 border-gray-600/40 rounded-lg p-6 mb-6 relative overflow-hidden">
              <p className="text-center text-sm text-gray-400 mb-4">
                Click the QR code to simulate payment
              </p>
              <div className="flex justify-center">
                <div 
                  onClick={handleQrClick}
                  className="w-64 h-64 bg-black/40 border-2 border-gray-600/40 rounded flex items-center justify-center cursor-pointer hover:bg-black/50 transition-colors"
                  title="Click to simulate payment"
                >
                  <p className="text-xs text-gray-300 text-center px-4 break-all">
                    {paymentQr}
                  </p>
                </div>
              </div>
            </div>

            {paymentStatus === 'pending' && (
              <div className="wood-panel border-2 border-yellow-500/50 rounded-lg p-4 text-center relative overflow-hidden">
                <p className="text-sm text-yellow-400">
                  Waiting for payment...
                </p>
              </div>
            )}
          </>
        )}
      </div>
    </div>
  );
}

