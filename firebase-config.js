<script type="module">
  // Import the functions you need from the SDKs you need
  import { initializeApp } from "https://www.gstatic.com/firebasejs/12.19.0/firebase-app.js";
  import { getAnalytics } from "https://www.gstatic.com/firebasejs/12.19.0/firebase-analytics.js";
  // TODO: Add SDKs for Firebase products that you want to use
  // https://firebase.google.com/docs/web/setup#available-libraries

  // Your web app's Firebase configuration
  // For Firebase JS SDK v7.20.0 and later, measurementId is optional
  const firebaseConfig = {
    apiKey: "AIzaSyDygWnHP5SWMXTpvI6O-7O0ioX76EJ-wk",
    authDomain: "smart-patient-assitant.firebaseapp.com",
    projectId: "smart-patient-assitant",
    storageBucket: "smart-patient-assitant.firebasestorage.app",
    messagingSenderId: "765449953775",
    appId: "1:765449953775:web:31c8ececf3c88cd779961a",
    measurementId: "G-B0XEVTCSYF"
  };

  // Initialize Firebase
  const app = initializeApp(firebaseConfig);
  const analytics = getAnalytics(app);
</script>